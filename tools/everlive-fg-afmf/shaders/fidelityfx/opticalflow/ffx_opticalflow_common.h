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

#if !defined(FFX_OPTICALFLOW_COMMON_H)
#define FFX_OPTICALFLOW_COMMON_H

#if defined(FFX_GPU)

#define SCD_OUTPUT_SCENE_CHANGE_SLOT            0
#define SCD_OUTPUT_HISTORY_BITS_SLOT            1
#define SCD_OUTPUT_COMPLETED_WORKGROUPS_SLOT    2


#define ffxClamp(x, a, b) (ffxMax(a, ffxMin(b, x)))


FfxUInt32 GetPackedLuma(FfxInt32 width, FfxInt32 x, FfxUInt32 luma0, FfxUInt32 luma1, FfxUInt32 luma2, FfxUInt32 luma3)
{
    FfxUInt32 packedLuma = luma0 | (luma1 << 8) | (luma2 << 16) | (luma3 << 24);

    if (x < 0)
    {
        FfxUInt32 outOfScreenFiller = packedLuma & 0xffu;
        if (x <= -1)
            packedLuma = (packedLuma << 8) | outOfScreenFiller;
        if (x <= -2)
            packedLuma = (packedLuma << 8) | outOfScreenFiller;
        if (x <= -3)
            packedLuma = (packedLuma << 8) | outOfScreenFiller;
    }
    else if (x > width - 4)
    {
        FfxUInt32 outOfScreenFiller = packedLuma & 0xff000000u;
        if (x >= width - 3)
            packedLuma = (packedLuma >> 8) | outOfScreenFiller;
        if (x >= width - 2)
            packedLuma = (packedLuma >> 8) | outOfScreenFiller;
        if (x >= width - 1)
            packedLuma = (packedLuma >> 8) | outOfScreenFiller;
    }
    return packedLuma;
}

#ifndef AFMF_SAD_INT16
#define AFMF_SAD_INT16 0
#endif
#if AFMF_SAD_INT16 == 1
// afmf-linux: the SAD on packed 16-bit pairs (two bytes per operation, v_pk_* on RDNA) instead
// of one byte at a time: the bytes at 0 and 2 of a word form one pair, 1 and 3 the other, and
// the difference of a pair is max - min, which needs no sign handling. Needs shaderInt16.
u16vec2 AfmfBytes02(FfxUInt32 a) { return unpack16(a & 0x00ff00ffu); }
u16vec2 AfmfBytes13(FfxUInt32 a) { return unpack16((a >> 8) & 0x00ff00ffu); }
u16vec2 AfmfAbsDiff(u16vec2 a, u16vec2 b) { return max(a, b) - min(a, b); }
FfxUInt32 AfmfSum(u16vec2 s)
{
    FfxUInt32 t = pack32(s);
    return (t & 0xffffu) + (t >> 16);
}
#endif

FfxUInt32 Sad(FfxUInt32 a, FfxUInt32 b)
{
#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1
    return msad4(a, FfxUInt32x2(b, 0), FfxUInt32x4(0, 0, 0, 0)).x;
#elif AFMF_SAD_INT16 == 1
    return AfmfSum(AfmfAbsDiff(AfmfBytes02(a), AfmfBytes02(b)) +
                   AfmfAbsDiff(AfmfBytes13(a), AfmfBytes13(b)));
#else
    return abs(FfxInt32((a >> 0) & 0xffu) - FfxInt32((b >> 0) & 0xffu)) +
        abs(FfxInt32((a >> 8) & 0xffu) - FfxInt32((b >> 8) & 0xffu)) +
        abs(FfxInt32((a >> 16) & 0xffu) - FfxInt32((b >> 16) & 0xffu)) +
        abs(FfxInt32((a >> 24) & 0xffu) - FfxInt32((b >> 24) & 0xffu));
#endif
}

FfxUInt32x4 QSad(FfxUInt32 a0, FfxUInt32 a1, FfxUInt32 b)
{
#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1
    return msad4(b, FfxUInt32x2(a0, a1), FfxUInt32x4(0, 0, 0, 0));
#else
    FfxUInt32x4 sad;
    sad.x = Sad(a0, b);

    a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
    a1 >>= 8;
    sad.y = Sad(a0, b);

    a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
    a1 >>= 8;
    sad.z = Sad(a0, b);

    a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
    sad.w = Sad(a0, b);
    return sad;
#endif
}

#endif  // #if defined(FFX_GPU)

#endif //!defined(FFX_OPTICALFLOW_COMMON_H)
