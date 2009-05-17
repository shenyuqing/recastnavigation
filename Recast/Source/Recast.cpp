//
// Copyright (c) 2009 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include <float.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "Recast.h"
#include "RecastLog.h"
#include "RecastTimer.h"


void rcIntArray::resize(int n)
{
	if (n > m_cap)
	{
		if (!m_cap) m_cap = 8;
		while (m_cap < n) m_cap *= 2;
		int* newData = new int[m_cap];
		if (m_size && newData) memcpy(newData, m_data, m_size*sizeof(int));
		delete [] m_data;
		m_data = newData;
	}
	m_size = n;
}
		
void rcCalcBounds(const float* verts, int nv, float* bmin, float* bmax)
{
	// Calculate bounding box.
	vcopy(bmin, verts);
	vcopy(bmax, verts);
	for (int i = 1; i < nv; ++i)
	{
		const float* v = &verts[i*3];
		vmin(bmin, v);
		vmax(bmax, v);
	}
}

void rcCalcGridSize(const float* bmin, const float* bmax, float cs, int* w, int* h)
{
	*w = (int)((bmax[0] - bmin[0])/cs+0.5f);
	*h = (int)((bmax[2] - bmin[2])/cs+0.5f);
}

bool rcCreateHeightfield(rcHeightfield& hf, int width, int height)
{
	hf.width = width;
	hf.height = height;
	hf.spans = new rcSpan*[hf.width*hf.height];
	if (!hf.spans)
		return false;
	memset(hf.spans, 0, sizeof(rcSpan*)*hf.width*hf.height);
	return true;
}

/*void rcMarkWalkableTriangles(const float walkableSlopeAngle,
							 const int* tris, const float* norms, int nt,
							 unsigned char* flags)
{
	const float walkableThr = cosf(walkableSlopeAngle/180.0f*(float)M_PI);
	
	for (int i = 0; i < nt; ++i)
	{
		// Check if the face is walkable.
		if (norms[i*3+1] > walkableThr)
			flags[i] |= RC_WALKABLE;
	}
}*/

static void calcTriNormal(const float* v0, const float* v1, const float* v2, float* norm)
{
	float e0[3], e1[3];
	vsub(e0, v1, v0);
	vsub(e1, v2, v0);
	vcross(norm, e0, e1);
	vnormalize(norm);
}

void rcMarkWalkableTriangles(const float walkableSlopeAngle,
							 const float* verts, int nv,
							 const int* tris, int nt,
							 unsigned char* flags)
{
	const float walkableThr = cosf(walkableSlopeAngle/180.0f*(float)M_PI);

	float norm[3];
	
	for (int i = 0; i < nt; ++i)
	{
		const int* tri = &tris[i*3];
		calcTriNormal(&verts[tri[0]*3], &verts[tri[1]*3], &verts[tri[2]*3], norm);
		// Check if the face is walkable.
		if (norm[1] > walkableThr)
			flags[i] |= RC_WALKABLE;
	}
}

static int getSpanCount(unsigned char flags, rcHeightfield& hf)
{
	const int w = hf.width;
	const int h = hf.height;
	int spanCount = 0;
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			for (rcSpan* s = hf.spans[x + y*w]; s; s = s->next)
			{
				if (s->flags == flags)
					spanCount++;
			}
		}
	}
	return spanCount;
}

inline void setCon(rcCompactSpan& s, int dir, int i)
{
	s.con &= ~(0xf << (dir*4));
	s.con |= (i&0xf) << (dir*4);
}

bool rcBuildCompactHeightfield(const float* bmin, const float* bmax,
							   const float cs, const float ch,
							   const int walkableHeight, const int walkableClimb,
							   unsigned char flags, rcHeightfield& hf,
							   rcCompactHeightfield& chf)
{
	rcTimeVal startTime = rcGetPerformanceTimer();
	
	const int w = hf.width;
	const int h = hf.height;
	const int spanCount = getSpanCount(flags, hf);

	// Fill in header.
	chf.width = w;
	chf.height = h;
	chf.spanCount = spanCount;
	chf.walkableHeight = walkableHeight;
	chf.walkableClimb = walkableClimb;
	chf.maxRegions = 0;
	vcopy(chf.bmin, bmin);
	vcopy(chf.bmax, bmax);
	chf.bmax[1] += walkableHeight*ch;
	chf.cs = cs;
	chf.ch = ch;
	chf.cells = new rcCompactCell[w*h];
	if (!chf.cells)
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildCompactHeightfield: Out of memory 'chf.cells' (%d)", w*h);
		return false;
	}
	memset(chf.cells, 0, sizeof(rcCompactCell)*w*h);
	chf.spans = new rcCompactSpan[spanCount];
	if (!chf.spans)
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildCompactHeightfield: Out of memory 'chf.spans' (%d)", spanCount);
		return false;
	}
	memset(chf.spans, 0, sizeof(rcCompactSpan)*spanCount);
	
	const int MAX_HEIGHT = 0xffff;
	
	// Fill in cells and spans.
	int idx = 0;
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const rcSpan* s = hf.spans[x + y*w];
			// If there are no spans at this cell, just leave the data to index=0, count=0.
			if (!s) continue;
			rcCompactCell& c = chf.cells[x+y*w];
			c.index = idx;
			c.count = 0;
			while (s)
			{
				if (s->flags == flags)
				{
					const int bot = (int)s->smax;
					const int top = (int)s->next ? (int)s->next->smin : MAX_HEIGHT;
					chf.spans[idx].y = (unsigned short)rcClamp(bot, 0, 0xffff);
					chf.spans[idx].h = (unsigned char)rcClamp(top - bot, 0, 0xff);
					idx++;
					c.count++;
				}
				s = s->next;
			}
		}
	}

	// Find neighbour connections.
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const rcCompactCell& c = chf.cells[x+y*w];
			for (int i = (int)c.index, ni = (int)(c.index+c.count); i < ni; ++i)
			{
				rcCompactSpan& s = chf.spans[i];
				for (int dir = 0; dir < 4; ++dir)
				{
					setCon(s, dir, 0xf);
					const int nx = x + rcGetDirOffsetX(dir);
					const int ny = y + rcGetDirOffsetY(dir);
					// First check that the neighbour cell is in bounds.
					if (nx < 0 || ny < 0 || nx >= w || ny >= h)
						continue;
					// Iterate over all neighbour spans and check if any of the is
					// accessible from current cell.
					const rcCompactCell& nc = chf.cells[nx+ny*w];
					for (int k = (int)nc.index, nk = (int)(nc.index+nc.count); k < nk; ++k)
					{
						const rcCompactSpan& ns = chf.spans[k];
						const int bot = rcMax(s.y, ns.y);
						const int top = rcMin(s.y+s.h, ns.y+ns.h);

						// Check that the gap between the spans is walkable,
						// and that the climb height between the gaps is not too high.
						if ((top - bot) >= walkableHeight && rcAbs((int)ns.y - (int)s.y) <= walkableClimb)
						{
							// Mark direction as walkable.
							setCon(s, dir, k - (int)nc.index);
							break;
						}
					}
				}
			}
		}
	}
	
	rcTimeVal endTime = rcGetPerformanceTimer();
	
//	if (rcGetLog())
//		rcGetLog()->log(RC_LOG_PROGRESS, "Build compact: %.3f ms", rcGetDeltaTimeUsec(startTime, endTime)/1000.0f);
	if (rcGetBuildTimes())
		rcGetBuildTimes()->buildCompact += rcGetDeltaTimeUsec(startTime, endTime);
	
	return true;
}

static int getHeightfieldMemoryUsage(const rcHeightfield& hf)
{
	int size = 0;
	size += sizeof(hf);
	size += hf.width * hf.height * sizeof(rcSpan*);
	
	rcSpanPool* pool = hf.pools;
	while (pool)
	{
		size += (sizeof(rcSpanPool) - sizeof(rcSpan)) + sizeof(rcSpan)*RC_SPANS_PER_POOL;
		pool = pool->next;
	}
	return size;
}

static int getCompactHeightFieldMemoryusage(const rcCompactHeightfield& chf)
{
	int size = 0;
	size += sizeof(rcCompactHeightfield);
	size += sizeof(rcCompactSpan) * chf.spanCount;
	size += sizeof(rcCompactCell) * chf.width * chf.height;
	return size;
}

bool rcBuildNavMesh(const rcConfig& cfg,
					const float* verts, const int nverts,
					const int* tris, const unsigned char* tflags, const int ntris,
					rcHeightfield& solid,
					rcCompactHeightfield& chf,
					rcContourSet& cset,
					rcPolyMesh& polyMesh)
{
	if (!rcCreateHeightfield(solid, cfg.width, cfg.height))
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildNavMesh: Could not create solid heightfield.");
		return false;
	}
	
	rcRasterizeTriangles(cfg.bmin, cfg.bmax, cfg.cs, cfg.ch,
						 verts, nverts, tris, tflags, ntris, solid);
	
	rcFilterWalkableBorderSpans(cfg.walkableHeight, cfg.walkableClimb, solid);
	
	rcFilterWalkableLowHeightSpans(cfg.walkableHeight, solid);

/*	if (!rcMarkReachableSpans(cfg.walkableHeight, cfg.walkableClimb, solid))
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildNavMesh: Could not build navigable heightfield.");
		return false;
	}*/
	
	if (!rcBuildCompactHeightfield(cfg.bmin, cfg.bmax, cfg.cs, cfg.ch,
								   cfg.walkableHeight, cfg.walkableClimb,
								   RC_WALKABLE/*|RC_REACHABLE*/, solid, chf))
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildNavMesh: Could not build compact data.");
		return false;
	}

	if (!rcBuildDistanceField(chf))
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildNavMesh: Could not build distance fields.");
		return false;
	}
	
	if (!rcBuildRegions(chf, cfg.walkableRadius, cfg.borderSize, cfg.minRegionSize, cfg.mergeRegionSize))
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildNavMesh: Could not build regions.");
		return false;
	}
	
	if (!rcBuildContours(chf, cfg.maxSimplificationError, cfg.maxEdgeLen, cset))
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildNavMesh: Could not create contours.");
		return false;
	}
	
	if (!rcBuildPolyMesh(cset, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch,
						 cfg.maxVertsPerPoly, polyMesh))
	{
		if (rcGetLog())
			rcGetLog()->log(RC_LOG_ERROR, "rcBuildNavMesh: Could not triangulate contours.");
		return false;
	}

	return true;
}
