/*
 *  Copyright (C) 2026, Benjamin Vernoux <bvernoux@hydrasdr.com>
 *
 *  Fast IEEE 754 math approximations for inspectrum ng.
 *  Used in spectrogram rendering, averaging, and reassignment.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

static_assert(std::numeric_limits<float>::is_iec559,
	"fastmath.h requires IEEE 754 binary32 floats");
static_assert(sizeof(float) == sizeof(int32_t),
	"fastmath.h requires sizeof(float) == sizeof(int32_t)");

/*
 * Fast IEEE 754 approximations for log2 and exp2.
 *
 * IEEE 754 float: [sign:1][exponent:8][mantissa:23]
 * For positive normal x: bits(x) ~= 2^23 * (log2(x) + 127)
 *
 * Max error: log2 ~0.086 log2 units (~0.26 dB), exp2 <7% relative.
 * A 256-color spectrogram over 100 dB has ~0.39 dB per step,
 * so the error is sub-pixel and visually identical to standard math.
 *
 * Preconditions:
 *   fast_log2f_approx: x > 0  (use linearTodB() for clamped variant)
 *   fast_exp2f_approx: |x| < 126 (clamped internally)
 */
static inline float fast_log2f_approx(float x)
{
	int32_t i;
	memcpy(&i, &x, sizeof(i));
	return (float)(i - 0x3F800000) * 1.1920928955078125e-7f; /* 1/(1<<23) */
}

static inline float fast_exp2f_approx(float x)
{
	/* clamp to keep (x * 2^23) within int32_t range with margin;
	 * +/- 126 is enough to cover normal floats and avoids UB on
	 * the float-to-int conversion for any caller-supplied value */
	if (x >  126.0f) x =  126.0f;
	if (x < -126.0f) x = -126.0f;
	int32_t i = (int32_t)(x * 8388608.0f) + 0x3F800000; /* x*(1<<23) + 127*(1<<23) */
	float r;
	memcpy(&r, &i, sizeof(r));
	return r;
}

/* dB conversion: dB = log2(power) * dBFS_SCALE */
static constexpr float dBFS_SCALE = 3.0102999566398120f;  /* 10/log2(10) */

/* linear conversion: power = exp2(dB * DB_TO_LIN) */
static constexpr float DB_TO_LIN  = 0.33219280948873626f; /* log2(10)/10 */

/* floor value for log-safe clamping (avoids log(0)) */
static constexpr float LOG_SAFE_FLOOR = 1e-30f;

/* dB <-> linear convenience wrappers */
static inline float dBtoLinear(float dB)
{
	return fast_exp2f_approx(dB * DB_TO_LIN);
}

static inline float linearTodB(float lin)
{
	if (lin < LOG_SAFE_FLOOR)
		lin = LOG_SAFE_FLOOR;
	return fast_log2f_approx(lin) * dBFS_SCALE;
}
