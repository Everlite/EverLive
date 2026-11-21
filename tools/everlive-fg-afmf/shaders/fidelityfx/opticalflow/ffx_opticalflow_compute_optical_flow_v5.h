// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef FFX_OPTICALFLOW_COMPUTE_OPTICAL_FLOW_V5_H
#define FFX_OPTICALFLOW_COMPUTE_OPTICAL_FLOW_V5_H

#define CompareSize (4 * 2)
#define BlockSizeY 8
#define BlockSizeX 8
#define ThreadCount (4 * 16)
#define SearchRadiusX (8)
#define SearchRadiusY (8)
#define BlockCount 2

#define SearchBufferSizeX ((CompareSize + SearchRadiusX*2)/4)
#define SearchBufferSizeY  (CompareSize + SearchRadiusY*2)

FFX_GROUPSHARED FfxUInt32 pixels[CompareSize][CompareSize / 4];
FFX_GROUPSHARED FfxUInt32 searchBuffer[1][SearchBufferSizeY * SearchBufferSizeX];
#define bankBreaker 1
FFX_GROUPSHARED FfxUInt32 sadMapBuffer[4][SearchRadiusY * 2][(SearchRadiusX * 2) / 4 + bankBreaker];

// afmf-linux: the group's ThreadCount lanes form however many subgroups the driver chose (one
// of 64 or two of 32 on RADV; up to eight of 8 on ANV), and the cross-subgroup step goes
// through shared memory indexed by the subgroup id, so every lane of the group ends with the
// same sum or minimum. The SDK combined exactly two subgroups of 32: with smaller ones the
// result differed between subgroups, the early-outs below split the group, and the next
// barrier waited forever (a GPU hang on Intel).
#define MaxWaves ThreadCount
FFX_GROUPSHARED FfxUInt32 sWaveSad[MaxWaves];
FFX_GROUPSHARED FfxUInt32 sWaveMin[MaxWaves];
#if defined(FFX_GLSL)
#define AfmfWaveCount() gl_NumSubgroups
#define AfmfWaveId() gl_SubgroupID
#else
#define AfmfWaveCount() (FfxUInt32(ThreadCount) / ffxWaveLaneCount())
#define AfmfWaveId() (FfxUInt32(iLocalIndex) / ffxWaveLaneCount())
#endif

// afmf-linux: a block whose 64 pixels differ from the previous frame's at the predicted vector
// (zero at the coarsest level, the coarser level's result below it) by no more than this (sum of
// absolute 8-bit luma differences) keeps that vector and skips the search. 0 disables it. Set by
// the layer at pipeline creation (AFMF_STATIC_BLOCK_SAD).
#if defined(FFX_GLSL)
layout(constant_id = 0) const FfxUInt32 afmfStaticBlockSad = 0u;
#else
static const FfxUInt32 afmfStaticBlockSad = 0u;
#endif

FfxUInt32 BlockSad64(FfxUInt32 blockSadSum, FfxInt32 iLocalIndex, FfxInt32 iLaneToBlockId, FfxInt32 block)
{
    if (iLaneToBlockId != block)
    {
        blockSadSum = 0u;
    }
    blockSadSum = ffxWaveSum(blockSadSum);

    if (AfmfWaveCount() > 1u)
    {
        if (ffxWaveIsFirstLane())
        {
            sWaveSad[AfmfWaveId()] = blockSadSum;
        }
        FFX_GROUP_MEMORY_BARRIER;
        blockSadSum = 0u;
        for (FfxUInt32 w = 0u; w < AfmfWaveCount(); w++)
        {
            blockSadSum += sWaveSad[w];
        }
    }

    return blockSadSum;
}

FfxUInt32 SadMapMinReduction256(FfxInt32x2 iSearchId, FfxInt32 iLocalIndex)
{
    FfxUInt32 min01 = ffxMin(sadMapBuffer[0][iSearchId.y][iSearchId.x], sadMapBuffer[1][iSearchId.y][iSearchId.x]);
    FfxUInt32 min23 = ffxMin(sadMapBuffer[2][iSearchId.y][iSearchId.x], sadMapBuffer[3][iSearchId.y][iSearchId.x]);
    FfxUInt32 min0123 = ffxMin(min01, min23);
    min0123 = ffxWaveMin(min0123);

    if (AfmfWaveCount() > 1u)
    {
        if (ffxWaveIsFirstLane())
        {
            sWaveMin[AfmfWaveId()] = min0123;
        }
        FFX_GROUP_MEMORY_BARRIER;
        for (FfxUInt32 w = 0u; w < AfmfWaveCount(); w++)
        {
            min0123 = ffxMin(min0123, sWaveMin[w]);
        }
    }

    return min0123;
}

void LoadSearchBuffer(FfxInt32 iLocalIndex, FfxInt32x2 iPxPosShifted)
{
    FfxInt32 baseX = (iPxPosShifted.x - SearchRadiusX);
    FfxInt32 baseY = (iPxPosShifted.y - SearchRadiusY);

    for (FfxInt32 id = iLocalIndex; id < SearchBufferSizeX * SearchBufferSizeY; id += ThreadCount)
    {
        FfxInt32 idx = id % SearchBufferSizeX;
        FfxInt32 idy = id / SearchBufferSizeX;
        FfxInt32 x = baseX + idx * 4;
        FfxInt32 y = baseY + idy;
        searchBuffer[0][id] = LoadSecondImagePackedLuma(FfxInt32x2(x, y));
    }
    FFX_GROUP_MEMORY_BARRIER;
}

FfxUInt32x4 CalculateQSads2(FfxInt32x2 iSearchId)
{
    FfxUInt32x4 sad = ffxBroadcast4(0u);

#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1

    FfxInt32 idx = iSearchId.y * 6 + iSearchId.x;

    sad = msad4(pixels[0][0], FfxUInt32x2(searchBuffer[0][idx],     searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[0][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[1][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[1][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[2][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[2][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[3][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[3][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[4][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[4][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[5][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[5][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[6][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[6][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[7][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[7][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);

#elif AFMF_SAD_INT16 == 1
    // afmf-linux: the four candidates' SADs accumulated as packed 16-bit pairs over the eight
    // rows (at most 8 x 4 x 255 per lane) and summed once at the end. The twelve bytes of a
    // row give nine (j, j + 2) byte pairs; candidate k compares pairs k and k + 1 against the
    // block's first word and pairs k + 4 and k + 5 against its second.
    u16vec2 acc0 = u16vec2(0), acc1 = u16vec2(0), acc2 = u16vec2(0), acc3 = u16vec2(0);
    for (FfxInt32 dy = 0; dy < CompareSize; dy++)
    {
        FfxInt32 rowOffset = (iSearchId.y + dy) * SearchBufferSizeX;
        FfxUInt32 a0 = searchBuffer[0][rowOffset + iSearchId.x];
        FfxUInt32 a1 = searchBuffer[0][rowOffset + iSearchId.x + 1];
        FfxUInt32 a2 = searchBuffer[0][rowOffset + iSearchId.x + 2];
        FfxUInt32 a01 = (a0 >> 16) | (a1 << 16);
        FfxUInt32 a12 = (a1 >> 16) | (a2 << 16);
        u16vec2 p0 = AfmfBytes02(a0), p1 = AfmfBytes13(a0);
        u16vec2 p2 = AfmfBytes02(a01), p3 = AfmfBytes13(a01);
        u16vec2 p4 = AfmfBytes02(a1), p5 = AfmfBytes13(a1);
        u16vec2 p6 = AfmfBytes02(a12), p7 = AfmfBytes13(a12);
        u16vec2 p8 = AfmfBytes02(a2);
        u16vec2 b0e = AfmfBytes02(pixels[dy][0]), b0o = AfmfBytes13(pixels[dy][0]);
        u16vec2 b1e = AfmfBytes02(pixels[dy][1]), b1o = AfmfBytes13(pixels[dy][1]);
        acc0 += AfmfAbsDiff(p0, b0e) + AfmfAbsDiff(p1, b0o) + AfmfAbsDiff(p4, b1e) + AfmfAbsDiff(p5, b1o);
        acc1 += AfmfAbsDiff(p1, b0e) + AfmfAbsDiff(p2, b0o) + AfmfAbsDiff(p5, b1e) + AfmfAbsDiff(p6, b1o);
        acc2 += AfmfAbsDiff(p2, b0e) + AfmfAbsDiff(p3, b0o) + AfmfAbsDiff(p6, b1e) + AfmfAbsDiff(p7, b1o);
        acc3 += AfmfAbsDiff(p3, b0e) + AfmfAbsDiff(p4, b0o) + AfmfAbsDiff(p7, b1e) + AfmfAbsDiff(p8, b1o);
    }
    sad = FfxUInt32x4(AfmfSum(acc0), AfmfSum(acc1), AfmfSum(acc2), AfmfSum(acc3));
#else
    for (FfxInt32 dy = 0; dy < CompareSize; dy++)
    {
        FfxInt32 rowOffset = (iSearchId.y + dy) * SearchBufferSizeX;
        FfxUInt32 a0 = searchBuffer[0][rowOffset + iSearchId.x];
        FfxUInt32 a1 = searchBuffer[0][rowOffset + iSearchId.x + 1];
        FfxUInt32 a2 = searchBuffer[0][rowOffset + iSearchId.x + 2];
        sad += QSad(a0, a1, pixels[dy][0]);
        sad += QSad(a1, a2, pixels[dy][1]);
    }
#endif

    return sad;
}

FfxUInt32x2 abs_2(FfxInt32x2 val)
{
    FfxInt32x2 tmp = val;
    FfxInt32x2 mask = tmp >> 31;
    FfxUInt32x2 res = (tmp + mask) ^ mask;
    return res;
}

FfxUInt32 EncodeSearchCoord(FfxInt32x2 coord)
{
#if FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
    FfxUInt32x2 absCoord = FfxUInt32x2(abs_2(coord - 8));
    return FfxUInt32(absCoord.y << 12) | FfxUInt32(absCoord.x << 8) | FfxUInt32(coord.y << 4) | FfxUInt32(coord.x);
#else //FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
    return FfxUInt32(coord.y << 8) | FfxUInt32(coord.x);
#endif //FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
}

FfxInt32x2 DecodeSearchCoord(FfxUInt32 bits)
{
#if FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
    FfxInt32 dx = FfxInt32(bits & 0xfu) - SearchRadiusX;
    FfxInt32 dy = FfxInt32((bits >> 4) & 0xfu) - SearchRadiusY;

    return FfxInt32x2(dx, dy);
#else
    FfxInt32 dx = FfxInt32(bits & 0xffu) - SearchRadiusX;
    FfxInt32 dy = FfxInt32((bits >> 8) & 0xffu) - SearchRadiusY;

    return FfxInt32x2(dx, dy);
#endif
}

void PrepareSadMap(FfxInt32x2 iSearchId, FfxUInt32x4 qsad)
{
    sadMapBuffer[0][iSearchId.y][iSearchId.x] = (qsad.x << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 0, iSearchId.y));
    sadMapBuffer[1][iSearchId.y][iSearchId.x] = (qsad.y << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 1, iSearchId.y));
    sadMapBuffer[2][iSearchId.y][iSearchId.x] = (qsad.z << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 2, iSearchId.y));
    sadMapBuffer[3][iSearchId.y][iSearchId.x] = (qsad.w << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 3, iSearchId.y));
    FFX_GROUP_MEMORY_BARRIER;
}


uint ABfe(uint src, uint off, uint bits) { uint mask = (1u << bits) - 1u; return (src >> off) & mask; }
uint ABfi(uint src, uint ins, uint mask) { return (ins & mask) | (src & (~mask)); }
uint ABfiM(uint src, uint ins, uint bits) { uint mask = (1u << bits) - 1u; return (ins & mask) | (src & (~mask)); }
void MapThreads(in FfxInt32x2 iGroupId, in FfxInt32 iLocalIndex,
                out FfxInt32x2 iSearchId, out FfxInt32x2 iPxPos, out FfxInt32 iLaneToBlockId)
{
    iSearchId = FfxInt32x2(ABfe(iLocalIndex, 0u, 2u), ABfe(iLocalIndex, 2u, 4u));
    iLaneToBlockId = FfxInt32(ABfe(iLocalIndex, 1u, 1u) | (ABfe(iLocalIndex, 5u, 1u) << 1u));
    iPxPos = (iGroupId << 4u) + iSearchId * FfxInt32x2(4, 1);
}

void ComputeOpticalFlowAdvanced(FfxInt32x2 iGlobalId, FfxInt32x2 iLocalId, FfxInt32x2 iGroupId, FfxInt32 iLocalIndex)
{
    FfxInt32x2 iSearchId;
    FfxInt32x2 iPxPos;
    FfxInt32 iLaneToBlockId;
    MapThreads(iGroupId, iLocalIndex, iSearchId, iPxPos, iLaneToBlockId);

    FfxInt32x2 currentOFPos = iPxPos >> 3u;

    if (IsSceneChanged())
    {
        if ((iSearchId.y & 0x7) == 0 && (iSearchId.x & 0x1) == 0)
        {
            StoreOpticalFlow(currentOFPos, FfxInt32x2(0, 0));
        }

        return;
    }

    const FfxBoolean bUsePredictionFromPreviousLevel = (OpticalFlowPyramidLevel() != OpticalFlowPyramidLevelCount() - 1);

    FfxUInt32 packedLuma_4blocks = LoadFirstImagePackedLuma(iPxPos);

#if FFX_LOCAL_SEARCH_FALLBACK == 1
    FfxUInt32 prevPackedLuma_4blocks = LoadSecondImagePackedLuma(iPxPos);
    FfxUInt32 sad_4blocks = Sad(packedLuma_4blocks, prevPackedLuma_4blocks);
#endif //FFX_LOCAL_SEARCH_FALLBACK

    FfxInt32x2 ofGroupOffset = iGroupId << 1u;
    FfxInt32x2 pixelGroupOffset = iGroupId << 4u;

#if FFX_LOCAL_SEARCH_FALLBACK == 1
    // afmf-linux: for each of the four blocks, its SAD at the vector predicted by the coarser
    // level (zero at the coarsest) and its SAD where it already is, all before the loop: every
    // lane belongs to one block, so it compares its four pixels against the previous frame at
    // both places, and one wave sum per block gives the block's two sums. The loop below uses
    // them to decide whether the block may skip its 256-candidate search, and afterwards to
    // decide whether the search's winner is worth believing. The sums are uniform across the
    // group, so the branches in the loop are too; the barriers keep one sum's cross-wave read
    // clear of the next one's write.
    FfxUInt32 predictedSad[4] = {0u, 0u, 0u, 0u};
    FfxUInt32 restSad[4] = {0u, 0u, 0u, 0u};
    {
        FfxInt32x2 laneBlock = FfxInt32x2(iLaneToBlockId & 1, iLaneToBlockId >> 1);
        FfxInt32x2 laneVector = bUsePredictionFromPreviousLevel ? LoadRwOpticalFlow(ofGroupOffset + laneBlock) : FfxInt32x2(0, 0);
        FfxUInt32 laneSad = (laneVector.x != 0 || laneVector.y != 0)
            ? Sad(packedLuma_4blocks, LoadSecondImagePackedLuma(iPxPos + laneVector))
            : sad_4blocks;
        // Both sums travel in one: a lane's SAD over its four pixels is at most 4 * 255, so a
        // block's 64 lanes reach 65280 and each half stays inside its 16 bits.
        FfxUInt32 lanePair = laneSad | (sad_4blocks << 16u);
        for (FfxInt32 b = 0; b < 4; b++)
        {
            FfxUInt32 pair = BlockSad64(lanePair, iLocalIndex, iLaneToBlockId, b);
            predictedSad[b] = pair & 0xffffu;
            restSad[b] = pair >> 16u;
            FFX_GROUP_MEMORY_BARRIER;
        }
    }
#endif //FFX_LOCAL_SEARCH_FALLBACK

    FfxInt32x2 blockId;
    for (blockId.y = 0; blockId.y < BlockCount; blockId.y++)
    {
        for (blockId.x = 0; blockId.x < BlockCount; blockId.x++)
        {
            FfxInt32x2 currentVector = LoadRwOpticalFlow(ofGroupOffset + blockId);
            if (!bUsePredictionFromPreviousLevel)
            {
                currentVector = FfxInt32x2(0, 0);
            }

#if FFX_LOCAL_SEARCH_FALLBACK == 1
            // afmf-linux: a block can skip its search two ways, and both need evidence. If it
            // matches the previous frame where it already is, it did not move: vector zero, and
            // the prediction is dropped. Otherwise the prediction is kept only when it matches
            // *and* is clearly better than not moving at all; without that second test a
            // textureless block (fog, snow, sky), where every candidate matches equally well,
            // keeps whatever vector the coarser level handed down and passes the mistake on.
            FfxInt32 blockIndex = blockId.x + blockId.y * 2;
            if (afmfStaticBlockSad != 0u && predictedSad[blockIndex] <= afmfStaticBlockSad &&
                predictedSad[blockIndex] * 2u <= restSad[blockIndex])
            {
                StoreOpticalFlow(ofGroupOffset + blockId, currentVector);
                continue;
            }
            if (afmfStaticBlockSad != 0u && restSad[blockIndex] <= afmfStaticBlockSad)
            {
                StoreOpticalFlow(ofGroupOffset + blockId, FfxInt32x2(0, 0));
                continue;
            }
#endif //FFX_LOCAL_SEARCH_FALLBACK

            if (iLaneToBlockId == blockId.y * 2 + blockId.x)
            {
                pixels[iSearchId.y & 0x7][iSearchId.x & 0x1] = packedLuma_4blocks;
            }

            LoadSearchBuffer(iLocalIndex, pixelGroupOffset + blockId * 8 + currentVector);

            FfxUInt32x4 qsad = CalculateQSads2(iSearchId);

            PrepareSadMap(iSearchId, qsad);
            FfxUInt32 minSad = SadMapMinReduction256(iSearchId, iLocalIndex);

            FfxInt32x2 minSadCoord = DecodeSearchCoord(minSad);
            FfxInt32x2 newVector = currentVector + minSadCoord;

#if FFX_LOCAL_SEARCH_FALLBACK == 1
            // afmf-linux: the winner has to earn the block. The SDK dropped it at level 0 when
            // staying put matched at least as well; here that applies at every level, and the
            // winner must be clearly better, not merely better: where the picture carries no
            // detail every candidate matches about as well as the rest, and the one that wins by
            // a hair is noise the interpolator would warp the background with. Its sum is the one
            // taken before the loop, so this costs no wave sum of its own.
            if ((minSad >> 16u) * 2u > restSad[blockIndex])
            {
                newVector = FfxInt32x2(0, 0);
            }
#endif //FFX_LOCAL_SEARCH_FALLBACK

            {
                StoreOpticalFlow(ofGroupOffset + blockId, newVector);
            }
        }
    }
}

#endif // FFX_OPTICALFLOW_COMPUTE_OPTICAL_FLOW_V5_H
