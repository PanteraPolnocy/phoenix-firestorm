/**
 * @file llsurfacepatch.cpp
 * @brief LLSurfacePatch class implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llsurfacepatch.h"
#include "llpatchvertexarray.h"
#include "llviewerobjectlist.h"
#include "llvosurfacepatch.h"
#include "llsurface.h"
#include "pipeline.h"
#include "llagent.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llregionhandle.h" // <FS:CR> Aurora Sim

// For getting composition values
#include "llviewerregion.h"
#include "llvlcomposition.h"
#include "lldrawpool.h"
#include "noise.h"

extern bool gShiftFrame;
extern U64MicrosecondsImplicit gFrameTime;
extern LLPipeline gPipeline;

LLSurfacePatch::LLSurfacePatch()
:   mHasReceivedData(false),
    mSTexUpdate(false),
    mDirty(false),
    mDirtyZStats(true),
    mHeightsGenerated(false),
    mDataOffset(0),
    mDataZ(NULL),
    mDataNorm(NULL),
    mVObjp(NULL),
    mOriginRegion(0.f, 0.f, 0.f),
    mCenterRegion(0.f, 0.f, 0.f),
    mMinZ(0.f),
    mMaxZ(0.f),
    mMeanZ(0.f),
    mRadius(0.f),
    mMinComposition(0.f),
    mMaxComposition(0.f),
    mMeanComposition(0.f),
    // This flag is used to communicate between adjacent surfaces and is
    // set to non-zero values by higher classes.
    mConnectedEdge(NO_EDGE),
    mLastUpdateTime(0),
    mSurfacep(NULL)
{
    S32 i;
    for (i = 0; i < 8; i++)
    {
        setNeighborPatch(i, NULL);
    }
    for (i = 0; i < 9; i++)
    {
        mNormalsInvalid[i] = true;
    }
}


LLSurfacePatch::~LLSurfacePatch()
{
    mVObjp = NULL;
}


void LLSurfacePatch::dirty()
{
    // These are outside of the loop in case we're still waiting for a dirty from the
    // texture being updated...
    if (mVObjp)
    {
        mVObjp->dirtyGeom();
    }
    else
    {
        LL_WARNS("Terrain") << "No viewer object for this surface patch!" << LL_ENDL;
    }

    mDirtyZStats = true;
    mHeightsGenerated = false;

    if (!mDirty)
    {
        mDirty = true;
        mSurfacep->dirtySurfacePatch(this);
    }
}


void LLSurfacePatch::setSurface(LLSurface *surfacep)
{
    mSurfacep = surfacep;
    if (mVObjp == (LLVOSurfacePatch *)NULL)
    {
        llassert(mSurfacep->mType == 'l');

        mVObjp = (LLVOSurfacePatch *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_SURFACE_PATCH, mSurfacep->getRegion());
        mVObjp->setPatch(this);
        mVObjp->setPositionRegion(mCenterRegion);
        gPipeline.createObject(mVObjp);
    }
}

void LLSurfacePatch::disconnectNeighbor(LLSurface *surfacep)
{
    U32 i;
    for (i = 0; i < 8; i++)
    {
        // <FS:Beq> cleanup disconnect logic a bit to see if it helps reduce OpenSim crashes.
        // if (getNeighborPatch(i))
        // {
        //  if (getNeighborPatch(i)->mSurfacep == surfacep)
        //  {
        if (auto patch = getNeighborPatch(i))
        {
            if (patch->mSurfacep == surfacep)
            {
                // Clean up connected edges
                switch(i)
                {
                    case EAST:
                        mConnectedEdge &= ~EAST_EDGE;
                        break;
                    case NORTH:
                        mConnectedEdge &= ~NORTH_EDGE;
                        break;
                    case WEST:
                        mConnectedEdge &= ~WEST_EDGE;
                        break;
                    case SOUTH:
                        mConnectedEdge &= ~SOUTH_EDGE;
                        break;
                }
        // </FS:Beq>
                setNeighborPatch(i, NULL);
                mNormalsInvalid[i] = true;
            }
        }
    }

    // <FS:Beq> cleanup disconnect logic a bit to see if it helps reduce OpenSim crashes.
    // // Clean up connected edges
    // if (getNeighborPatch(EAST))
    // {
    //  if (getNeighborPatch(EAST)->mSurfacep == surfacep)
    //  {
    //      mConnectedEdge &= ~EAST_EDGE;
    //  }
    // }
    // if (getNeighborPatch(NORTH))
    // {
    //  if (getNeighborPatch(NORTH)->mSurfacep == surfacep)
    //  {
    //      mConnectedEdge &= ~NORTH_EDGE;
    //  }
    // }
    // if (getNeighborPatch(WEST))
    // {
    //  if (getNeighborPatch(WEST)->mSurfacep == surfacep)
    //  {
    //      mConnectedEdge &= ~WEST_EDGE;
    //  }
    // }
    // if (getNeighborPatch(SOUTH))
    // {
    //  if (getNeighborPatch(SOUTH)->mSurfacep == surfacep)
    //  {
    //      mConnectedEdge &= ~SOUTH_EDGE;
    //  }
    // }
    // </FS:Beq>
}

LLVector3 LLSurfacePatch::getPointAgent(const U32 x, const U32 y) const
{
    U32 surface_stride = mSurfacep->getGridsPerEdge();
    U32 point_offset = x + y*surface_stride;
    LLVector3 pos;
    pos = getOriginAgent();
    pos.mV[VX] += x * mSurfacep->getMetersPerGrid();
    pos.mV[VY] += y * mSurfacep->getMetersPerGrid();
    pos.mV[VZ] = *(mDataZ + point_offset);
    return pos;
}

LLVector2 LLSurfacePatch::getTexCoords(const U32 x, const U32 y) const
{
    U32 surface_stride = mSurfacep->getGridsPerEdge();
    U32 point_offset = x + y*surface_stride;
    LLVector3 pos, rel_pos;
    pos = getOriginAgent();
    pos.mV[VX] += x * mSurfacep->getMetersPerGrid();
    pos.mV[VY] += y * mSurfacep->getMetersPerGrid();
    pos.mV[VZ] = *(mDataZ + point_offset);
    rel_pos = pos - mSurfacep->getOriginAgent();
    rel_pos *= 1.f/surface_stride;
    return LLVector2(rel_pos.mV[VX], rel_pos.mV[VY]);
}


void LLSurfacePatch::eval(const U32 x, const U32 y, const U32 stride, LLVector3 *vertex, LLVector3 *normal,
                          LLVector2* tex0, LLVector2 *tex1) const
{
    if (!mSurfacep || !mSurfacep->getRegion() || !mSurfacep->getGridsPerEdge() || !mVObjp)
    {
        return; // failsafe
    }
    llassert_always(vertex && normal && tex1);

    U32 surface_stride = mSurfacep->getGridsPerEdge();
    U32 point_offset = x + y*surface_stride;

    *normal = getNormal(x, y);

    LLVector3 pos_agent = getOriginAgent();
    pos_agent.mV[VX] += x * mSurfacep->getMetersPerGrid();
    pos_agent.mV[VY] += y * mSurfacep->getMetersPerGrid();
    pos_agent.mV[VZ]  = *(mDataZ + point_offset);
    *vertex     = pos_agent-mVObjp->getRegion()->getOriginAgent();

    // tex0 is used for ownership overlay
    LLVector3 rel_pos = pos_agent - mSurfacep->getOriginAgent();
    LLVector3 tex_pos = rel_pos * (1.f / (surface_stride * mSurfacep->getMetersPerGrid()));
    tex0->mV[0] = tex_pos.mV[0];
    tex0->mV[1] = tex_pos.mV[1];

    tex1->mV[0] = mSurfacep->getRegion()->getCompositionXY(llfloor(mOriginRegion.mV[0])+x, llfloor(mOriginRegion.mV[1])+y);

    const F32 xyScale = 4.9215f*7.f; //0.93284f;
    const F32 xyScaleInv = (1.f / xyScale)*(0.2222222222f);

    F32 vec[3] = {
                    (F32)fmod((F32)(mOriginGlobal.mdV[0] + x)*xyScaleInv, 256.f),
                    (F32)fmod((F32)(mOriginGlobal.mdV[1] + y)*xyScaleInv, 256.f),
                    0.f
                };
    F32 rand_val = llclamp(noise2(vec)* 0.75f + 0.5f, 0.f, 1.f);
    tex1->mV[1] = rand_val;


}


template<>
void LLSurfacePatch::calcNormal</*PBR=*/false>(const U32 x, const U32 y, const U32 stride)
{
    U32 patch_width = mSurfacep->mPVArray.mPatchWidth;
    U32 surface_stride = mSurfacep->getGridsPerEdge();

    const F32 mpg = mSurfacep->getMetersPerGrid() * stride;

// <FS:CR> Aurora Sim
    //S32 poffsets[2][2][2];
    S32 poffsets[2][2][3];
// </FS:CR> Aurora Sim
    poffsets[0][0][0] = x - stride;
    poffsets[0][0][1] = y - stride;
// <FS:CR> Aurora Sim
    poffsets[0][0][2] = surface_stride;
// </FS:CR> Aurora Sim

    poffsets[0][1][0] = x - stride;
    poffsets[0][1][1] = y + stride;
// <FS:CR> Aurora Sim
    poffsets[0][1][2] = surface_stride;
// </FS:CR> Aurora Sim

    poffsets[1][0][0] = x + stride;
    poffsets[1][0][1] = y - stride;
// <FS:CR> Aurora Sim
    poffsets[1][0][2] = surface_stride;
// </FS:CR> Aurora Sim

    poffsets[1][1][0] = x + stride;
    poffsets[1][1][1] = y + stride;
// <FS:CR> Aurora Sim
    poffsets[1][1][2] = surface_stride;
// </FS:CR> Aurora Sim

    const LLSurfacePatch *ppatches[2][2];

    // LLVector3 p1, p2, p3, p4;

    ppatches[0][0] = this;
    ppatches[0][1] = this;
    ppatches[1][0] = this;
    ppatches[1][1] = this;

    U32 i, j;
    for (i = 0; i < 2; i++)
    {
        for (j = 0; j < 2; j++)
        {
            if (poffsets[i][j][0] < 0)
            {
                if (!ppatches[i][j]->getNeighborPatch(WEST))
                {
                    poffsets[i][j][0] = 0;
                }
                else
                {
// <FS:CR> Aurora Sim
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(WEST);
                    poffsets[i][j][0] += patch_width;
                    poffsets[i][j][2] = ppatches[i][j]->getSurface()->getGridsPerEdge();
// </FS:CR> Aurora Sim
                }
            }
            if (poffsets[i][j][1] < 0)
            {
                if (!ppatches[i][j]->getNeighborPatch(SOUTH))
                {
                    poffsets[i][j][1] = 0;
                }
                else
                {
// <FS:CR> Aurora Sim
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(SOUTH);
                    poffsets[i][j][1] += patch_width;
                    poffsets[i][j][2] = ppatches[i][j]->getSurface()->getGridsPerEdge();
// </FS>CR> Aurora Sim
                }
            }
            if (poffsets[i][j][0] >= (S32)patch_width)
            {
                if (!ppatches[i][j]->getNeighborPatch(EAST))
                {
                    poffsets[i][j][0] = patch_width - 1;
                }
                else
                {
// <FS:CR> Aurora Sim
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(EAST);
                    poffsets[i][j][0] -= patch_width;
                    poffsets[i][j][2] = ppatches[i][j]->getSurface()->getGridsPerEdge();
// </FS:CR> Aurora Sim
                }
            }
            if (poffsets[i][j][1] >= (S32)patch_width)
            {
                if (!ppatches[i][j]->getNeighborPatch(NORTH))
                {
                    poffsets[i][j][1] = patch_width - 1;
                }
                else
                {
// <FS:CR> Aurora Sim
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(NORTH);
                    poffsets[i][j][1] -= patch_width;
                    poffsets[i][j][2] = ppatches[i][j]->getSurface()->getGridsPerEdge();
// </FS:CR> Aurora Sim
                }
            }
        }
    }

    LLVector3 p00(-mpg,-mpg,
                  *(ppatches[0][0]->mDataZ
                  + poffsets[0][0][0]
// <FS:CR> Aurora Sim
                  //+ poffsets[0][0][1]*surface_stride));
                  + poffsets[0][0][1]*poffsets[0][0][2]));
// </FS:CR> Aurora Sim
    LLVector3 p01(-mpg,+mpg,
                  *(ppatches[0][1]->mDataZ
                  + poffsets[0][1][0]
// <FS:CR> Aurora Sim
                  //+ poffsets[0][1][1]*surface_stride));
                  + poffsets[0][1][1]*poffsets[0][1][2]));
// </FS:CR> Aurora Sim
    LLVector3 p10(+mpg,-mpg,
                  *(ppatches[1][0]->mDataZ
                  + poffsets[1][0][0]
// <FS:CR> Aurora Sim
                  //+ poffsets[1][0][1]*surface_stride));
                  + poffsets[1][0][1]*poffsets[1][0][2]));
// </FS:CR> Aurora Sim
    LLVector3 p11(+mpg,+mpg,
                  *(ppatches[1][1]->mDataZ
                  + poffsets[1][1][0]
// <FS:CR> Aurora Sim
                  //+ poffsets[1][1][1]*surface_stride));
                  + poffsets[1][1][1]*poffsets[1][1][2]));
// </FS:CR> Aurora Sim

    LLVector3 c1 = p11 - p00;
    LLVector3 c2 = p01 - p10;

    LLVector3 normal = c1;
    normal %= c2;
    normal.normVec();

    llassert(mDataNorm);
    *(mDataNorm + surface_stride * y + x) = normal;
}

template<>
void LLSurfacePatch::calcNormal</*PBR=*/true>(const U32 x, const U32 y, const U32 stride)
{
    llassert(mDataNorm);
    constexpr U32 index = 0;

    const U32 surface_stride = mSurfacep->getGridsPerEdge();
    LLVector3& normal_out = *(mDataNorm + surface_stride * y + x);
    calcNormalFlat(normal_out, x, y, index);
}

// Calculate the flat normal of a triangle whose least coordinate is specified by the given x,y values.
// If index = 0, calculate the normal of the first triangle, otherwise calculate the normal of the second.
void LLSurfacePatch::calcNormalFlat(LLVector3& normal_out, const U32 x, const U32 y, const U32 index)
{
    llassert(index == 0 || index == 1);

    U32 patch_width = mSurfacep->mPVArray.mPatchWidth;
    U32 surface_stride = mSurfacep->getGridsPerEdge();

    // Vertex stride is always 1 because we want the flat surface of the current triangle face
    constexpr U32 stride = 1;

    const F32 mpg = mSurfacep->getMetersPerGrid() * stride;

    S32 poffsets[2][2][2];
    poffsets[0][0][0] = x;
    poffsets[0][0][1] = y;

    poffsets[0][1][0] = x;
    poffsets[0][1][1] = y + stride;

    poffsets[1][0][0] = x + stride;
    poffsets[1][0][1] = y;

    poffsets[1][1][0] = x + stride;
    poffsets[1][1][1] = y + stride;

    const LLSurfacePatch *ppatches[2][2];

    // LLVector3 p1, p2, p3, p4;

    ppatches[0][0] = this;
    ppatches[0][1] = this;
    ppatches[1][0] = this;
    ppatches[1][1] = this;

    U32 i, j;
    for (i = 0; i < 2; i++)
    {
        for (j = 0; j < 2; j++)
        {
            if (poffsets[i][j][0] < 0)
            {
                if (!ppatches[i][j]->getNeighborPatch(WEST))
                {
                    poffsets[i][j][0] = 0;
                }
                else
                {
                    poffsets[i][j][0] += patch_width;
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(WEST);
                }
            }
            if (poffsets[i][j][1] < 0)
            {
                if (!ppatches[i][j]->getNeighborPatch(SOUTH))
                {
                    poffsets[i][j][1] = 0;
                }
                else
                {
                    poffsets[i][j][1] += patch_width;
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(SOUTH);
                }
            }
            if (poffsets[i][j][0] >= (S32)patch_width)
            {
                if (!ppatches[i][j]->getNeighborPatch(EAST))
                {
                    poffsets[i][j][0] = patch_width - 1;
                }
                else
                {
                    poffsets[i][j][0] -= patch_width;
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(EAST);
                }
            }
            if (poffsets[i][j][1] >= (S32)patch_width)
            {
                if (!ppatches[i][j]->getNeighborPatch(NORTH))
                {
                    poffsets[i][j][1] = patch_width - 1;
                }
                else
                {
                    poffsets[i][j][1] -= patch_width;
                    ppatches[i][j] = ppatches[i][j]->getNeighborPatch(NORTH);
                }
            }
        }
    }

    LLVector3 p00(-mpg,-mpg,
                  *(ppatches[0][0]->mDataZ
                  + poffsets[0][0][0]
                  + poffsets[0][0][1]*surface_stride));
    LLVector3 p01(-mpg,+mpg,
                  *(ppatches[0][1]->mDataZ
                  + poffsets[0][1][0]
                  + poffsets[0][1][1]*surface_stride));
    LLVector3 p10(+mpg,-mpg,
                  *(ppatches[1][0]->mDataZ
                  + poffsets[1][0][0]
                  + poffsets[1][0][1]*surface_stride));
    LLVector3 p11(+mpg,+mpg,
                  *(ppatches[1][1]->mDataZ
                  + poffsets[1][1][0]
                  + poffsets[1][1][1]*surface_stride));

    // Triangle index / coordinate convention
    // for a single surface patch
    //
    // p01          p11
    //
    // ^   ._____.
    // |   |\    |
    // |   | \ 1 |
    // |   |  \  |
    //     | 0 \ |
    // y   |____\|
    //
    // p00  x --->  p10
    //
    // (z up / out of the screen due to right-handed coordinate system)

    LLVector3 normal;
    if (index == 0)
    {
        LLVector3 c1 = p10 - p00;
        LLVector3 c2 = p01 - p00;

        normal = c1;
        normal %= c2;
        normal.normVec();
    }
    else // index == 1
    {
        LLVector3 c1 = p11 - p01;
        LLVector3 c2 = p11 - p10;

        normal = c1;
        normal %= c2;
        normal.normVec();
    }

    llassert(&normal_out);
    normal_out = normal;
}

const LLVector3 &LLSurfacePatch::getNormal(const U32 x, const U32 y) const
{
    U32 surface_stride = mSurfacep->getGridsPerEdge();
    llassert(mDataNorm);
    return *(mDataNorm + surface_stride * y + x);
}


void LLSurfacePatch::updateCameraDistanceRegion(const LLVector3 &pos_region)
{
    if (LLPipeline::sDynamicLOD)
    {
        if (!gShiftFrame)
        {
            LLVector3 dv = pos_region;
            dv -= mCenterRegion;
            mVisInfo.mDistance = llmax(0.f, (F32)(dv.magVec() - mRadius))/
                llmax(LLVOSurfacePatch::sLODFactor, 0.1f);
        }
    }
    else
    {
        mVisInfo.mDistance = 0.f;
    }
}

F32 LLSurfacePatch::getDistance() const
{
    return mVisInfo.mDistance;
}


// Called when a patch has changed its height field
// data.
void LLSurfacePatch::updateVerticalStats()
{
    if (!mDirtyZStats)
    {
        return;
    }

    U32 grids_per_patch_edge = mSurfacep->getGridsPerPatchEdge();
    U32 grids_per_edge = mSurfacep->getGridsPerEdge();
    F32 meters_per_grid = mSurfacep->getMetersPerGrid();

    U32 i, j, k;
    F32 z, total;

    llassert(mDataZ);
    z = *(mDataZ);

    mMinZ = z;
    mMaxZ = z;

    k = 0;
    total = 0.0f;

    // Iterate to +1 because we need to do the edges correctly.
    for (j=0; j<(grids_per_patch_edge+1); j++)
    {
        for (i=0; i<(grids_per_patch_edge+1); i++)
        {
            z = *(mDataZ + i + j*grids_per_edge);

            if (z < mMinZ)
            {
                mMinZ = z;
            }
            if (z > mMaxZ)
            {
                mMaxZ = z;
            }
            total += z;
            k++;
        }
    }
    mMeanZ = total / (F32) k;
    mCenterRegion.mV[VZ] = 0.5f * (mMinZ + mMaxZ);

    LLVector3 diam_vec(meters_per_grid*grids_per_patch_edge,
                        meters_per_grid*grids_per_patch_edge,
                        mMaxZ - mMinZ);
    mRadius = diam_vec.magVec() * 0.5f;

    mSurfacep->mMaxZ = llmax(mMaxZ, mSurfacep->mMaxZ);
    mSurfacep->mMinZ = llmin(mMinZ, mSurfacep->mMinZ);
    mSurfacep->mHasZData = true;
    mSurfacep->getRegion()->calculateCenterGlobal();

    if (mVObjp)
    {
        mVObjp->dirtyPatch();
    }
    mDirtyZStats = false;
}


template<bool PBR>
void LLSurfacePatch::updateNormals()
{
    if (mSurfacep->mType == 'w')
    {
        return;
    }
    U32 grids_per_patch_edge = mSurfacep->getGridsPerPatchEdge();
    U32 grids_per_edge = mSurfacep->getGridsPerEdge();

    bool dirty_patch = false;

    U32 i, j;
    // update the east edge
    if (mNormalsInvalid[EAST] || mNormalsInvalid[NORTHEAST] || mNormalsInvalid[SOUTHEAST])
    {
        for (j = 0; j <= grids_per_patch_edge; j++)
        {
            calcNormal<PBR>(grids_per_patch_edge, j, 2);
            calcNormal<PBR>(grids_per_patch_edge - 1, j, 2);
            calcNormal<PBR>(grids_per_patch_edge - 2, j, 2);
        }

        dirty_patch = true;
    }

    // update the north edge
    if (mNormalsInvalid[NORTHEAST] || mNormalsInvalid[NORTH] || mNormalsInvalid[NORTHWEST])
    {
// <FS:CR> Aurora Sim
        /*
        if(!getNeighborPatch(EAST) && getNeighborPatch(NORTHEAST))
        {
            if(getNeighborPatch(NORTHEAST)->getHasReceivedData())
            {
                *(getNeighborPatch(NORTHEAST)->mDataZ) = 100.0f;
            }
        }
        */
// </FS:CR> Aurora Sim

        for (i = 0; i <= grids_per_patch_edge; i++)
        {
            calcNormal<PBR>(i, grids_per_patch_edge, 2);
            calcNormal<PBR>(i, grids_per_patch_edge - 1, 2);
            calcNormal<PBR>(i, grids_per_patch_edge - 2, 2);
        }

        dirty_patch = true;
    }

    // update the west edge
    if (mNormalsInvalid[NORTHWEST] || mNormalsInvalid[WEST] || mNormalsInvalid[SOUTHWEST])
    {
// <FS:CR> Aurora Sim
        if(!getNeighborPatch(NORTH) && getNeighborPatch(NORTHWEST))
        {
            if(getNeighborPatch(NORTHWEST)->getHasReceivedData())
            {
                *(mDataZ + grids_per_patch_edge*grids_per_edge) = *(getNeighborPatch(NORTHWEST)->mDataZ + grids_per_patch_edge);
            }
        }
// </FS:CR> Aurora Sim

        for (j = 0; j < grids_per_patch_edge; j++)
        {
            calcNormal<PBR>(0, j, 2);
            calcNormal<PBR>(1, j, 2);
        }
        dirty_patch = true;
    }

    // update the south edge
    if (mNormalsInvalid[SOUTHWEST] || mNormalsInvalid[SOUTH] || mNormalsInvalid[SOUTHEAST])
    {
// <FS:CR> Aurora Sim
        if(!getNeighborPatch(EAST) && getNeighborPatch(SOUTHEAST))
        {
            if(getNeighborPatch(SOUTHEAST)->getHasReceivedData())
            {
                *(mDataZ + grids_per_patch_edge) =
                *(getNeighborPatch(SOUTHEAST)->mDataZ + grids_per_patch_edge * getNeighborPatch(SOUTHEAST)->getSurface()->getGridsPerEdge());
            }
        }
// </FS:CR> Aurora Sim

        for (i = 0; i < grids_per_patch_edge; i++)
        {
            calcNormal<PBR>(i, 0, 2);
            calcNormal<PBR>(i, 1, 2);
        }
        dirty_patch = true;
    }

    // Invalidating the northeast corner is different, because depending on what the adjacent neighbors are,
    // we'll want to do different things.
    if (mNormalsInvalid[NORTHEAST])
    {
        if (!getNeighborPatch(NORTHEAST))
        {
            if (!getNeighborPatch(NORTH))
            {
                if (!getNeighborPatch(EAST))
                {
                    // No north or east neighbors.  Pull from the diagonal in your own patch.
                    *(mDataZ + grids_per_patch_edge + grids_per_patch_edge*grids_per_edge) =
                        *(mDataZ + grids_per_patch_edge - 1 + (grids_per_patch_edge - 1)*grids_per_edge);
                }
                else
                {
                    if (getNeighborPatch(EAST)->getHasReceivedData())
                    {
                        // East, but not north.  Pull from your east neighbor's northwest point.
                        *(mDataZ + grids_per_patch_edge + grids_per_patch_edge*grids_per_edge) =
// <FS:CR> Aurora Sim
                            //*(getNeighborPatch(EAST)->mDataZ + (grids_per_patch_edge - 1)*grids_per_edge);
                            *(getNeighborPatch(EAST)->mDataZ + (getNeighborPatch(EAST)->getSurface()->getGridsPerPatchEdge() - 1)*getNeighborPatch(EAST)->getSurface()->getGridsPerEdge());
// </FS:CR> Aurora Sim
                    }
                    else
                    {
                        *(mDataZ + grids_per_patch_edge + grids_per_patch_edge*grids_per_edge) =
                            *(mDataZ + grids_per_patch_edge - 1 + (grids_per_patch_edge - 1)*grids_per_edge);
                    }
                }
            }
            else
            {
                // We have a north.
                if (getNeighborPatch(EAST))
                {
                    // North and east neighbors, but not northeast.
                    // Pull from diagonal in your own patch.
                    *(mDataZ + grids_per_patch_edge + grids_per_patch_edge*grids_per_edge) =
                        *(mDataZ + grids_per_patch_edge - 1 + (grids_per_patch_edge - 1)*grids_per_edge);
                }
                else
                {
                    if (getNeighborPatch(NORTH)->getHasReceivedData())
                    {
                        // North, but not east.  Pull from your north neighbor's southeast corner.
                        *(mDataZ + grids_per_patch_edge + grids_per_patch_edge*grids_per_edge) =
// <FS:CR> Aurora Sim
                            //*(getNeighborPatch(NORTH)->mDataZ + (grids_per_patch_edge - 1));
                            *(getNeighborPatch(NORTH)->mDataZ + (getNeighborPatch(NORTH)->getSurface()->getGridsPerPatchEdge() - 1));
// </FS:CR> Aurora Sim
                    }
                    else
                    {
                        *(mDataZ + grids_per_patch_edge + grids_per_patch_edge*grids_per_edge) =
                            *(mDataZ + grids_per_patch_edge - 1 + (grids_per_patch_edge - 1)*grids_per_edge);
                    }
                }
            }
        }
        else if (getNeighborPatch(NORTHEAST)->mSurfacep != mSurfacep)
        {
            if (
                (!getNeighborPatch(NORTH) || (getNeighborPatch(NORTH)->mSurfacep != mSurfacep))
                &&
                (!getNeighborPatch(EAST) || (getNeighborPatch(EAST)->mSurfacep != mSurfacep)))
            {
// <FS:CR> Aurora Sim
                U32 own_xpos, own_ypos, neighbor_xpos, neighbor_ypos;
                S32 own_offset = 0, neighbor_offset = 0;
                from_region_handle(mSurfacep->getRegion()->getHandle(), &own_xpos, &own_ypos);
                from_region_handle(getNeighborPatch(NORTHEAST)->mSurfacep->getRegion()->getHandle(), &neighbor_xpos, &neighbor_ypos);
                if(own_ypos >= neighbor_ypos) {
                    neighbor_offset = own_ypos - neighbor_ypos;
                }
                else {
                    own_offset = neighbor_ypos - own_ypos;
                }
// </FS:CR> Aurora Sim

                *(mDataZ + grids_per_patch_edge + grids_per_patch_edge*grids_per_edge) =
                                        *(getNeighborPatch(NORTHEAST)->mDataZ +
// <FS:CR> Aurora Sim
                                            (grids_per_edge + neighbor_offset - own_offset - 1) *
                                            getNeighborPatch(NORTHEAST)->getSurface()->getGridsPerEdge() );
// </FS:CR> Aurora Sim
            }
        }
        else
        {
            // We've got a northeast patch in the same surface.
            // The z and normals will be handled by that patch.
        }
        calcNormal<PBR>(grids_per_patch_edge, grids_per_patch_edge, 2);
        calcNormal<PBR>(grids_per_patch_edge, grids_per_patch_edge - 1, 2);
        calcNormal<PBR>(grids_per_patch_edge - 1, grids_per_patch_edge, 2);
        calcNormal<PBR>(grids_per_patch_edge - 1, grids_per_patch_edge - 1, 2);
        dirty_patch = true;
    }

    // update the middle normals
    if (mNormalsInvalid[MIDDLE])
    {
        for (j=2; j < grids_per_patch_edge - 2; j++)
        {
            for (i=2; i < grids_per_patch_edge - 2; i++)
            {
                calcNormal<PBR>(i, j, 2);
            }
        }
        dirty_patch = true;
    }

    if (dirty_patch)
    {
        mSurfacep->dirtySurfacePatch(this);
    }

    for (i = 0; i < 9; i++)
    {
        mNormalsInvalid[i] = false;
    }
}

template void LLSurfacePatch::updateNormals</*PBR=*/false>();
template void LLSurfacePatch::updateNormals</*PBR=*/true>();

void LLSurfacePatch::updateEastEdge()
{
    U32 grids_per_patch_edge = mSurfacep->getGridsPerPatchEdge();
    U32 grids_per_edge = mSurfacep->getGridsPerEdge();
// <FS:CR> Aurora Sim
    U32 grids_per_edge_east = grids_per_edge;

    //U32 j, k;
    U32 j, k, h;
// <FS:CR> Aurora Sim
    F32 *west_surface, *east_surface;

    if (!getNeighborPatch(EAST))
    {
        west_surface = mDataZ + grids_per_patch_edge;
        east_surface = mDataZ + grids_per_patch_edge - 1;
    }
    else if (mConnectedEdge & EAST_EDGE)
    {
        west_surface = mDataZ + grids_per_patch_edge;
        east_surface = getNeighborPatch(EAST)->mDataZ;
// <FS:CR> Aurora Sim
        grids_per_edge_east = getNeighborPatch(EAST)->getSurface()->getGridsPerEdge();
// <FS:CR> Aurora Sim
    }
    else
    {
        return;
    }

    // If patchp is on the east edge of its surface, then we update the east
    // side buffer
    for (j=0; j < grids_per_patch_edge; j++)
    {
        k = j * grids_per_edge;
// <FS:CR> Aurora Sim
        h = j * grids_per_edge_east;
        *(west_surface + k) = *(east_surface + h);  // update buffer Z
        //*(west_surface + k) = *(east_surface + k);    // update buffer Z
// </FS:CR> Aurora Sim
    }
}


void LLSurfacePatch::updateNorthEdge()
{
    U32 grids_per_patch_edge = mSurfacep->getGridsPerPatchEdge();
    U32 grids_per_edge = mSurfacep->getGridsPerEdge();

    U32 i;
    F32 *south_surface, *north_surface;

    if (!getNeighborPatch(NORTH))
    {
        south_surface = mDataZ + grids_per_patch_edge*grids_per_edge;
        north_surface = mDataZ + (grids_per_patch_edge - 1) * grids_per_edge;
    }
    else if (mConnectedEdge & NORTH_EDGE)
    {
        south_surface = mDataZ + grids_per_patch_edge*grids_per_edge;
        north_surface = getNeighborPatch(NORTH)->mDataZ;
    }
    else
    {
        return;
    }

    // Update patchp's north edge ...
    for (i = 0; i<grids_per_patch_edge; i++)
    {
        *(south_surface + i) = *(north_surface + i);    // update buffer Z
    }
}

bool LLSurfacePatch::updateTexture()
{
    if (mSTexUpdate)        //  Update texture as needed
    {
        F32 meters_per_grid = getSurface()->getMetersPerGrid();
        F32 grids_per_patch_edge = (F32)getSurface()->getGridsPerPatchEdge();

        if ((!getNeighborPatch(EAST) || getNeighborPatch(EAST)->getHasReceivedData())
            && (!getNeighborPatch(WEST) || getNeighborPatch(WEST)->getHasReceivedData())
            && (!getNeighborPatch(SOUTH) || getNeighborPatch(SOUTH)->getHasReceivedData())
            && (!getNeighborPatch(NORTH) || getNeighborPatch(NORTH)->getHasReceivedData()))
        {
            LLViewerRegion *regionp = getSurface()->getRegion();
            LLVector3d origin_region = getOriginGlobal() - getSurface()->getOriginGlobal();

            // Have to figure out a better way to deal with these edge conditions...
            LLVLComposition* comp = regionp->getComposition();
            if (!mHeightsGenerated)
            {
                F32 patch_size = meters_per_grid*(grids_per_patch_edge+1);
                if (comp->generateHeights((F32)origin_region[VX], (F32)origin_region[VY],
                                          patch_size, patch_size))
                {
                    mHeightsGenerated = true;
                }
                else
                {
                    return false;
                }
            }

            if (comp->generateComposition())
            {
                if (mVObjp)
                {
                    mVObjp->dirtyGeom();
                    gPipeline.markGLRebuild(mVObjp);
                    return !mSTexUpdate;
                }
            }
        }
        return false;
    }
    else
    {
        return true;
    }
}

void LLSurfacePatch::updateGL()
{
    LL_PROFILE_ZONE_SCOPED;
    F32 meters_per_grid = getSurface()->getMetersPerGrid();
    F32 grids_per_patch_edge = (F32)getSurface()->getGridsPerPatchEdge();

    LLViewerRegion *regionp = getSurface()->getRegion();
    LLVector3d origin_region = getOriginGlobal() - getSurface()->getOriginGlobal();

    LLVLComposition* comp = regionp->getComposition();

    updateCompositionStats();
    F32 tex_patch_size = meters_per_grid*grids_per_patch_edge;

    mSTexUpdate = false;
}

void LLSurfacePatch::dirtyZ()
{
    mSTexUpdate = true;

    // Invalidate all normals in this patch
    U32 i;
    for (i = 0; i < 9; i++)
    {
        mNormalsInvalid[i] = true;
    }

    // Invalidate normals in this and neighboring patches
    for (i = 0; i < 8; i++)
    {
        if (getNeighborPatch(i))
        {
            getNeighborPatch(i)->mNormalsInvalid[gDirOpposite[i]] = true;
            getNeighborPatch(i)->dirty();
            if (i < 4)
            {
                getNeighborPatch(i)->mNormalsInvalid[gDirAdjacent[gDirOpposite[i]][0]] = true;
                getNeighborPatch(i)->mNormalsInvalid[gDirAdjacent[gDirOpposite[i]][1]] = true;
            }
        }
    }

    dirty();
    mLastUpdateTime = gFrameTime;
}


const U64 &LLSurfacePatch::getLastUpdateTime() const
{
    return mLastUpdateTime;
}

F32 LLSurfacePatch::getMaxZ() const
{
    return mMaxZ;
}

F32 LLSurfacePatch::getMinZ() const
{
    return mMinZ;
}

void LLSurfacePatch::setOriginGlobal(const LLVector3d &origin_global)
{
    mOriginGlobal = origin_global;

    LLVector3 origin_region;
    origin_region.setVec(mOriginGlobal - mSurfacep->getOriginGlobal());

    mOriginRegion = origin_region;
    mCenterRegion.mV[VX] = origin_region.mV[VX] + 0.5f*mSurfacep->getGridsPerPatchEdge()*mSurfacep->getMetersPerGrid();
    mCenterRegion.mV[VY] = origin_region.mV[VY] + 0.5f*mSurfacep->getGridsPerPatchEdge()*mSurfacep->getMetersPerGrid();

    mVisInfo.mbIsVisible = false;
    mVisInfo.mDistance = 512.0f;
    mVisInfo.mRenderLevel = 0;
    mVisInfo.mRenderStride = mSurfacep->getGridsPerPatchEdge();

}

void LLSurfacePatch::connectNeighbor(LLSurfacePatch *neighbor_patchp, const U32 direction)
{
    llassert(neighbor_patchp);
    mNormalsInvalid[direction] = true;
    neighbor_patchp->mNormalsInvalid[gDirOpposite[direction]] = true;

    setNeighborPatch(direction, neighbor_patchp);
    neighbor_patchp->setNeighborPatch(gDirOpposite[direction], this);

    if (EAST == direction)
    {
        mConnectedEdge |= EAST_EDGE;
        neighbor_patchp->mConnectedEdge |= WEST_EDGE;
    }
    else if (NORTH == direction)
    {
        mConnectedEdge |= NORTH_EDGE;
        neighbor_patchp->mConnectedEdge |= SOUTH_EDGE;
    }
    else if (WEST == direction)
    {
        mConnectedEdge |= WEST_EDGE;
        neighbor_patchp->mConnectedEdge |= EAST_EDGE;
    }
    else if (SOUTH == direction)
    {
        mConnectedEdge |= SOUTH_EDGE;
        neighbor_patchp->mConnectedEdge |= NORTH_EDGE;
    }
}

void LLSurfacePatch::updateVisibility()
{
    if (mVObjp.isNull())
    {
        return;
    }

    const F32 DEFAULT_DELTA_ANGLE   = (0.15f);
    U32 old_render_stride, max_render_stride;
    U32 new_render_level;
    F32 stride_per_distance = DEFAULT_DELTA_ANGLE / mSurfacep->getMetersPerGrid();
    U32 grids_per_patch_edge = mSurfacep->getGridsPerPatchEdge();

    LLVector4a center;
    center.load3( (mCenterRegion + mSurfacep->getOriginAgent()).mV);
    LLVector4a radius;
    radius.splat(mRadius);

    // sphere in frustum on global coordinates
    if (LLViewerCamera::getInstance()->AABBInFrustumNoFarClip(center, radius))
    {
        // We now need to calculate the render stride based on patchp's distance
        // from LLCamera render_stride is governed by a relation something like this...
        //
        //                       delta_angle * patch.distance
        // render_stride <=  ----------------------------------------
        //                           mMetersPerGrid
        //
        // where 'delta_angle' is the desired solid angle of the average polgon on a patch.
        //
        // Any render_stride smaller than the RHS would be 'satisfactory'.  Smaller
        // strides give more resolution, but efficiency suggests that we use the largest
        // of the render_strides that obey the relation.  Flexibility is achieved by
        // modulating 'delta_angle' until we have an acceptable number of triangles.

        old_render_stride = mVisInfo.mRenderStride;

        // Calculate the render_stride using information in agent
        max_render_stride = lltrunc(mVisInfo.mDistance * stride_per_distance);
        max_render_stride = llmin(max_render_stride , 2*grids_per_patch_edge);

        // We only use render_strides that are powers of two, so we use look-up tables to figure out
        // the render_level and corresponding render_stride
        new_render_level = mVisInfo.mRenderLevel = mSurfacep->getRenderLevel(max_render_stride);
        mVisInfo.mRenderStride = mSurfacep->getRenderStride(new_render_level);

        if ((mVisInfo.mRenderStride != old_render_stride))
            // The reason we check !mbIsVisible is because non-visible patches normals
            // are not updated when their data is changed.  When this changes we can get
            // rid of mbIsVisible altogether.
        {
            if (mVObjp)
            {
                mVObjp->dirtyGeom();
                // <FS:Ansariel> FIRE-19720: Crash when teleporting on Littlefield grid - LLVOSurfacePatch::dirtyGeom()
                //if (getNeighborPatch(WEST))
                //{
                //  getNeighborPatch(WEST)->mVObjp->dirtyGeom();
                //}
                //if (getNeighborPatch(SOUTH))
                //{
                //  getNeighborPatch(SOUTH)->mVObjp->dirtyGeom();
                //}
                LLSurfacePatch* neighbor = getNeighborPatch(WEST);
                if (neighbor && neighbor->mVObjp.notNull())
                {
                    neighbor->mVObjp->dirtyGeom();
                }
                neighbor = getNeighborPatch(SOUTH);
                if (neighbor && neighbor->mVObjp.notNull())
                {
                    neighbor->mVObjp->dirtyGeom();
                }
                // </FS:Ansariel>
            }
        }
        mVisInfo.mbIsVisible = true;
    }
    else
    {
        mVisInfo.mbIsVisible = false;
    }
}


const LLVector3d &LLSurfacePatch::getOriginGlobal() const
{
    return mOriginGlobal;
}

LLVector3 LLSurfacePatch::getOriginAgent() const
{
    return gAgent.getPosAgentFromGlobal(mOriginGlobal);
}

bool LLSurfacePatch::getVisible() const
{
    return mVisInfo.mbIsVisible;
}

U32 LLSurfacePatch::getRenderStride() const
{
    return mVisInfo.mRenderStride;
}

S32 LLSurfacePatch::getRenderLevel() const
{
    return mVisInfo.mRenderLevel;
}

void LLSurfacePatch::setHasReceivedData()
{
    mHasReceivedData = true;
}

bool LLSurfacePatch::getHasReceivedData() const
{
    return mHasReceivedData;
}

const LLVector3 &LLSurfacePatch::getCenterRegion() const
{
    return mCenterRegion;
}


void LLSurfacePatch::updateCompositionStats()
{
    LLViewerLayer *vlp = mSurfacep->getRegion()->getComposition();

    F32 x, y, width, height, mpg, min, mean, max;

    LLVector3 origin = getOriginAgent() - mSurfacep->getOriginAgent();
    mpg = mSurfacep->getMetersPerGrid();
    x = origin.mV[VX];
    y = origin.mV[VY];
    width = mpg*(mSurfacep->getGridsPerPatchEdge()+1);
    height = mpg*(mSurfacep->getGridsPerPatchEdge()+1);

    mean = 0.f;
    min = vlp->getValueScaled(x, y);
    max= min;
    U32 count = 0;
    F32 i, j;
    for (j = 0; j < height; j += mpg)
    {
        for (i = 0; i < width; i += mpg)
        {
            F32 comp = vlp->getValueScaled(x + i, y + j);
            mean += comp;
            min = llmin(min, comp);
            max = llmax(max, comp);
            count++;
        }
    }
    mean /= count;

    mMinComposition = min;
    mMeanComposition = mean;
    mMaxComposition = max;
}

F32 LLSurfacePatch::getMeanComposition() const
{
    return mMeanComposition;
}

F32 LLSurfacePatch::getMinComposition() const
{
    return mMinComposition;
}

F32 LLSurfacePatch::getMaxComposition() const
{
    return mMaxComposition;
}

void LLSurfacePatch::setNeighborPatch(const U32 direction, LLSurfacePatch *neighborp)
{
    mNeighborPatches[direction] = neighborp;
    mNormalsInvalid[direction] = true;
    if (direction < 4)
    {
        mNormalsInvalid[gDirAdjacent[direction][0]] = true;
        mNormalsInvalid[gDirAdjacent[direction][1]] = true;
    }
}

LLSurfacePatch *LLSurfacePatch::getNeighborPatch(const U32 direction) const
{
    return mNeighborPatches[direction];
}

void LLSurfacePatch::clearVObj()
{
    mVObjp = NULL;
}
