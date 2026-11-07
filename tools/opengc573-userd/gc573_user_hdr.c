/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_hdr.c — HDR10/HLG to SDR tonemapping for the EverLive streamer
 *
 * Why this shape and not a full RGB pipeline:
 *
 * A correct per-pixel path (YCbCr -> RGB -> PQ-EOTF -> tonemap -> BT.709 ->
 * gamma -> YCbCr) costs ~30 million table lookups and ~100 million FLOPs at
 * 1440p, which is 10-20 ms per frame on one core - far outside a 120 Hz
 * budget.  Because Y' already carries the luma, the whole transfer chain
 * (EOTF, tonemap, SDR OETF, limited-range scaling) collapses into a single
 * 256-entry table indexed by the byte that is already there.  Chroma only
 * needs the BT.2020 -> BT.709 conversion, which is a constant 2x2 matrix on
 * the centred Cb/Cr values.
 *
 * That leaves one table lookup and four multiplies per two pixels: the pass is
 * memory-bandwidth bound (1440p YUYV is 7.2 MB, 4K is 16.6 MB) rather than
 * compute bound, and it produces identical output for the gaming window and
 * the V4L2 loopback feed.
 *
 * Known approximation: the chroma matrix is applied to non-linear Cb/Cr
 * (the exact conversion needs linear light).  For game content the error is
 * small, and the saturation factor keeps wide-gamut BT.2020 colours from
 * hard-clipping after the matrix expands them into the smaller BT.709 gamut.
 */

#include "gc573_user_hdr.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#if defined(__linux__) && !defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#endif

#define HDR_LUT_SIZE		256
/* Y and Cb/Cr use limited range in the FPGA output. */
#define HDR_Y_OFFSET		16.0
#define HDR_Y_RANGE		219.0
#define HDR_C_OFFSET		128.0
#define HDR_C_MIN		16
#define HDR_C_MAX		240

/*
 * Saturation trim for the BT.2020 -> BT.709 gamut reduction.
 * 1.15f restores punchy, vibrant game colours in SDR without gamut oversaturation.
 */
#define HDR_SATURATION		1.15f

/* BT.2020 -> BT.709 (linear light), ITU-R BT.2087. */
static const double k_matrix_2020_to_709[3][3] = {
	{  1.6605, -0.5876, -0.0728 },
	{ -0.1246,  1.1329, -0.0083 },
	{ -0.0182, -0.1006,  1.1187 },
};
static const double k_y709[3] = { 0.2126, 0.7152, 0.0722 };

static uint8_t g_y_lut[HDR_LUT_SIZE];
static float g_cb_cb;
static float g_cb_cr;
static float g_cr_cb;
static float g_cr_cr;
static float g_m_cb_cb;
static float g_m_cb_cr;
static float g_m_cr_cb;
static float g_m_cr_cr;
static int g_active;
static bool g_passthrough;
static enum gc573_hdr_type g_type;
static float g_exposure = 1.0f;
static int g_init_done;
static void (*g_tonemap_impl)(void *frame, size_t macropixels) = NULL;

/* ------------------------------------------------------------------ */
/* Transfer functions                                                  */
/* ------------------------------------------------------------------ */

/* SMPTE ST 2084 EOTF: normalised signal -> cd/m^2. */
static double pq_eotf(double n)
{
	const double m1 = 2610.0 / 16384.0;
	const double m2 = 2523.0 / 32.0;
	const double c1 = 3424.0 / 4096.0;
	const double c2 = 2413.0 / 128.0;
	const double c3 = 2392.0 / 128.0;
	double p = pow(n, 1.0 / m2);
	double num = p - c1;
	double den = c2 - c3 * p;

	if (num < 0.0)
		num = 0.0;
	if (den <= 0.0)
		return 10000.0;

	return 10000.0 * pow(num / den, 1.0 / m1);
}

/* BT.2100 HLG inverse OETF: normalised signal -> linear (1.0 = 1000 cd/m^2). */
static double hlg_eotf(double e)
{
	const double a = 0.17883277;
	const double b = 1.0 - 4.0 * a;
	const double c = 0.5 - a * log(4.0 * a);

	if (e <= 0.5)
		return e * e / 3.0;
	return (exp((e - c) / a) + b) / 12.0;
}

/*
 * Filmic curve (John Hable / Uncharted 2 fit) followed by the BT.709 OETF.
 * Delivers punchy midtone contrast and deep shadows without crushing, while
 * smoothly rolling off highlights up to 1500+ cd/m^2 into the SDR ceiling.
 */
static double hable_partial(double x)
{
	const double a = 0.15; /* Shoulder strength */
	const double b = 0.50; /* Linear strength */
	const double c = 0.10; /* Linear angle */
	const double d = 0.20; /* Toe strength */
	const double e = 0.02; /* Toe numerator */
	const double f = 0.30; /* Toe denominator */

	return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - (e / f);
}

static double tonemap_encode(double x, double exposure)
{
	static double white_scale = 0.0;
	double m;

	if (white_scale == 0.0)
		white_scale = 1.0 / hable_partial(11.2);

	x *= (2.0 * exposure);
	if (x <= 0.0)
		return 0.0;

	m = hable_partial(x) * white_scale;
	if (m < 0.0)
		m = 0.0;
	if (m > 1.0)
		m = 1.0;

	if (m < 0.018)
		return 4.5 * m;
	return 1.099 * pow(m, 0.45) - 0.099;
}

static void hdr_build_lut(enum gc573_hdr_type type, double exposure)
{
	for (int i = 0; i < HDR_LUT_SIZE; i++) {
		double n = ((double)i - HDR_Y_OFFSET) / HDR_Y_RANGE;
		double linear;
		double out;

		if (n < 0.0)
			n = 0.0;
		if (n > 1.0)
			n = 1.0;

		if (type == GC573_HDR_PQ)
			linear = pq_eotf(n) / 203.0; /* ITU-R BT.2408 reference white: 203 cd/m^2 */
		else
			linear = hlg_eotf(n) * 3.5;

		out = tonemap_encode(linear, exposure);
		if (out < 0.0)
			out = 0.0;
		if (out > 1.0)
			out = 1.0;

		g_y_lut[i] = (uint8_t)(HDR_Y_OFFSET + out * HDR_Y_RANGE + 0.5);
	}
}

static inline uint8_t clamp_chroma(float value)
{
	int v = (int)(value + 0.5f);

	if (v < HDR_C_MIN)
		v = HDR_C_MIN;
	if (v > HDR_C_MAX)
		v = HDR_C_MAX;
	return (uint8_t)v;
}

static void tonemap_yuyv_scalar(void *frame, size_t macropixels)
{
	uint8_t *p = frame;

	for (size_t i = 0; i < macropixels; i++, p += 4) {
		float cb = (float)p[1] - HDR_C_OFFSET;
		float cr = (float)p[3] - HDR_C_OFFSET;

		p[0] = g_y_lut[p[0]];
		p[2] = g_y_lut[p[2]];
		p[1] = clamp_chroma(g_m_cb_cb * cb + g_m_cb_cr * cr + HDR_C_OFFSET);
		p[3] = clamp_chroma(g_m_cr_cb * cb + g_m_cr_cr * cr + HDR_C_OFFSET);
	}
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
static void tonemap_yuyv_sse2(void *frame, size_t macropixels)
{
	uint8_t *p = frame;
	size_t pairs = macropixels / 2;
	const __m128 v_offset_round = _mm_set1_ps(HDR_C_OFFSET + 0.5f);
	const __m128 v_c_min = _mm_set1_ps((float)HDR_C_MIN);
	const __m128 v_c_max = _mm_set1_ps((float)HDR_C_MAX);
	const __m128 v_k_cb = _mm_set_ps(g_m_cr_cb, g_m_cb_cb, g_m_cr_cb, g_m_cb_cb);
	const __m128 v_k_cr = _mm_set_ps(g_m_cr_cr, g_m_cb_cr, g_m_cr_cr, g_m_cb_cr);

	for (size_t i = 0; i < pairs; i++, p += 8) {
		p[0] = g_y_lut[p[0]];
		p[2] = g_y_lut[p[2]];
		p[4] = g_y_lut[p[4]];
		p[6] = g_y_lut[p[6]];

		float cb0 = (float)p[1] - HDR_C_OFFSET;
		float cr0 = (float)p[3] - HDR_C_OFFSET;
		float cb1 = (float)p[5] - HDR_C_OFFSET;
		float cr1 = (float)p[7] - HDR_C_OFFSET;

		__m128 v_cb = _mm_set_ps(cb1, cb1, cb0, cb0);
		__m128 v_cr = _mm_set_ps(cr1, cr1, cr0, cr0);

		__m128 val = _mm_add_ps(_mm_add_ps(_mm_mul_ps(v_cb, v_k_cb), _mm_mul_ps(v_cr, v_k_cr)), v_offset_round);
		val = _mm_max_ps(val, v_c_min);
		val = _mm_min_ps(val, v_c_max);
		__m128i ival = _mm_cvttps_epi32(val);

		__m128i p16 = _mm_packs_epi32(ival, ival);
		__m128i p8 = _mm_packus_epi16(p16, p16);
		uint32_t c_packed = (uint32_t)_mm_cvtsi128_si32(p8);

		p[1] = (uint8_t)c_packed;
		p[3] = (uint8_t)(c_packed >> 8);
		p[5] = (uint8_t)(c_packed >> 16);
		p[7] = (uint8_t)(c_packed >> 24);
	}

	if (macropixels & 1) {
		float cb = (float)p[1] - HDR_C_OFFSET;
		float cr = (float)p[3] - HDR_C_OFFSET;

		p[0] = g_y_lut[p[0]];
		p[2] = g_y_lut[p[2]];
		p[1] = clamp_chroma(g_m_cb_cb * cb + g_m_cb_cr * cr + HDR_C_OFFSET);
		p[3] = clamp_chroma(g_m_cr_cb * cb + g_m_cr_cr * cr + HDR_C_OFFSET);
	}
}
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
static void tonemap_yuyv_neon(void *frame, size_t macropixels)
{
	uint8_t *p = frame;
	size_t pairs = macropixels / 2;
	float32x4_t v_offset = vdupq_n_f32(HDR_C_OFFSET);
	float32x4_t v_c_min = vdupq_n_f32((float)HDR_C_MIN);
	float32x4_t v_c_max = vdupq_n_f32((float)HDR_C_MAX);
	float32x4_t v_k_cb = { g_m_cb_cb, g_m_cr_cb, g_m_cb_cb, g_m_cr_cb };
	float32x4_t v_k_cr = { g_m_cb_cr, g_m_cr_cr, g_m_cb_cr, g_m_cr_cr };

	for (size_t i = 0; i < pairs; i++, p += 8) {
		p[0] = g_y_lut[p[0]];
		p[2] = g_y_lut[p[2]];
		p[4] = g_y_lut[p[4]];
		p[6] = g_y_lut[p[6]];

		float cb0 = (float)p[1] - HDR_C_OFFSET;
		float cr0 = (float)p[3] - HDR_C_OFFSET;
		float cb1 = (float)p[5] - HDR_C_OFFSET;
		float cr1 = (float)p[7] - HDR_C_OFFSET;

		float32x4_t v_cb = { cb0, cb0, cb1, cb1 };
		float32x4_t v_cr = { cr0, cr0, cr1, cr1 };

		float32x4_t val = vmlaq_f32(vmlaq_f32(v_offset, v_cb, v_k_cb), v_cr, v_k_cr);
		val = vmaxq_f32(val, v_c_min);
		val = vminq_f32(val, v_c_max);

		int32x4_t ival = vcvtaq_s32_f32(val);
		int16x4_t p16 = vmovn_s32(ival);
		uint8x8_t p8 = vqmovun_s16(vcombine_s16(p16, p16));
		uint32_t c_packed = vget_lane_u32((uint32x2_t)p8, 0);

		p[1] = (uint8_t)c_packed;
		p[3] = (uint8_t)(c_packed >> 8);
		p[5] = (uint8_t)(c_packed >> 16);
		p[7] = (uint8_t)(c_packed >> 24);
	}

	if (macropixels & 1) {
		float cb = (float)p[1] - HDR_C_OFFSET;
		float cr = (float)p[3] - HDR_C_OFFSET;

		p[0] = g_y_lut[p[0]];
		p[2] = g_y_lut[p[2]];
		p[1] = clamp_chroma(g_m_cb_cb * cb + g_m_cb_cr * cr + HDR_C_OFFSET);
		p[3] = clamp_chroma(g_m_cr_cb * cb + g_m_cr_cr * cr + HDR_C_OFFSET);
	}
}
#endif

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void gc573_hdr_init(void)
{
	double w[3];
	double a_b[3];
	double a_r[3];
	int i, j;

	if (g_init_done)
		return;

	/* w = M^T * y709: the BT.709 luma of the converted primaries. */
	for (i = 0; i < 3; i++) {
		w[i] = 0.0;
		for (j = 0; j < 3; j++)
			w[i] += k_matrix_2020_to_709[j][i] * k_y709[j];
	}

	for (i = 0; i < 3; i++) {
		a_b[i] = k_matrix_2020_to_709[2][i] - w[i];
		a_r[i] = k_matrix_2020_to_709[0][i] - w[i];
	}

	/*
	 * With R' = Y' + 1.5816 Cr', B' = Y' + 1.9404 Cb' and
	 * G' = Y' - 0.6129 Cr' - 0.1697 Cb', the Y' term cancels because every
	 * row of M sums to 1, leaving a pure 2x2 chroma transform.
	 */
	g_cb_cb = (float)((-0.1697 * a_b[1] + 1.9404 * a_b[2]) / 1.8556);
	g_cb_cr = (float)((1.5816 * a_b[0] - 0.6129 * a_b[1]) / 1.8556);
	g_cr_cb = (float)((-0.1697 * a_r[1] + 1.9404 * a_r[2]) / 1.5748);
	g_cr_cr = (float)((1.5816 * a_r[0] - 0.6129 * a_r[1]) / 1.5748);

	g_m_cb_cb = g_cb_cb * HDR_SATURATION;
	g_m_cb_cr = g_cb_cr * HDR_SATURATION;
	g_m_cr_cb = g_cr_cb * HDR_SATURATION;
	g_m_cr_cr = g_cr_cr * HDR_SATURATION;

	g_tonemap_impl = tonemap_yuyv_scalar;
#if defined(__x86_64__) || defined(_M_X64)
	/* x86_64 baseline always includes SSE2 */
	g_tonemap_impl = tonemap_yuyv_sse2;
#elif defined(__i386__) || defined(_M_IX86)
	if (__builtin_cpu_supports("sse2"))
		g_tonemap_impl = tonemap_yuyv_sse2;
#elif defined(__aarch64__)
	/* AArch64 baseline always includes NEON */
	g_tonemap_impl = tonemap_yuyv_neon;
#elif defined(__ARM_NEON)
	g_tonemap_impl = tonemap_yuyv_neon;
#elif defined(__linux__) && defined(__arm__)
	if (getauxval(AT_HWCAP) & HWCAP_NEON)
		g_tonemap_impl = tonemap_yuyv_neon;
#endif

	g_init_done = 1;
}

void gc573_hdr_set_mode(enum gc573_hdr_type type, float exposure)
{
	if (!g_init_done)
		gc573_hdr_init();

	if (exposure < 0.25f)
		exposure = 0.25f;
	if (exposure > 4.0f)
		exposure = 4.0f;

	if (type == g_type && exposure == g_exposure) {
		g_active = type != GC573_HDR_SDR;
		return;
	}

	g_type = type;
	g_exposure = exposure;
	g_active = type != GC573_HDR_SDR;

	if (g_active)
		hdr_build_lut(type, (double)exposure);
}

bool gc573_hdr_active(void)
{
	return g_active != 0;
}

void gc573_hdr_set_passthrough(bool enable)
{
	g_passthrough = enable;
}

bool gc573_hdr_is_passthrough(void)
{
	return g_passthrough;
}

const char *gc573_hdr_label(void)
{
	if (!g_active)
		return "SDR";

	if (g_passthrough) {
		switch (g_type) {
		case GC573_HDR_PQ:
			return "HDR10 Passthrough (BT.2020 PQ)";
		case GC573_HDR_HLG:
			return "HLG Passthrough (BT.2100)";
		default:
			return "SDR";
		}
	}

	switch (g_type) {
	case GC573_HDR_PQ:
		return "HDR10 -> SDR Tonemapped";
	case GC573_HDR_HLG:
		return "HLG -> SDR Tonemapped";
	default:
		return "SDR";
	}
}

void gc573_hdr_tonemap_yuyv(void *frame, unsigned int width,
			    unsigned int height)
{
	size_t macropixels;

	if (!g_active || g_passthrough || !frame || width < 2U || !height)
		return;

	if (!g_init_done)
		gc573_hdr_init();

	/* Width is always even for YUYV; a trailing odd pixel is not reachable. */
	macropixels = (size_t)(width / 2U) * height;

	if (g_tonemap_impl)
		g_tonemap_impl(frame, macropixels);
	else
		tonemap_yuyv_scalar(frame, macropixels);
}
