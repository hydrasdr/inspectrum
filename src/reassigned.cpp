/*
 *  Copyright (C) 2026, Benjamin Vernoux <bvernoux@hydrasdr.com>
 *
 *  Reassigned and synchrosqueezed spectrogram for inspectrum ng.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "reassigned.h"
#include "fft.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <vector>

static const float TWO_PI_F = (float)Tau;
static const float FLOOR_DB = -200.0f;

void generateDerivativeWindow(const float *window, int windowSize, float *dest)
{
	if (windowSize <= 0)
		return;
	if (windowSize == 1) {
		dest[0] = 0.0f;
		return;
	}

	dest[0] = window[1] - window[0];
	for (int i = 1; i < windowSize - 1; i++)
		dest[i] = (window[i + 1] - window[i - 1]) * 0.5f;
	dest[windowSize - 1] = window[windowSize - 1] - window[windowSize - 2];
}

void generateTimeRampedWindow(const float *window, int windowSize, float *dest)
{
	float halfN = windowSize * 0.5f;
	for (int i = 0; i < windowSize; i++)
		dest[i] = (i - halfN) * window[i];
}

/* Apply window, zero-pad, execute FFT. Returns FFT internal buffer. */
static std::complex<float> *computeColumn(
	FFT &fft, const float *samples, size_t nSamples,
	size_t centerSample, int windowSize, int fftSize,
	const float *win,
	std::complex<float> *fftBuf)
{
	ptrdiff_t start = (ptrdiff_t)centerSample - windowSize / 2;

	for (int i = 0; i < windowSize; i++) {
		ptrdiff_t idx = start + i;
		if (idx < 0 || (size_t)idx >= nSamples) {
			fftBuf[i] = {0.0f, 0.0f};
		} else {
			float re = samples[idx * 2];
			float im = samples[idx * 2 + 1];
			fftBuf[i] = {re * win[i], im * win[i]};
		}
	}

	if (fftSize > windowSize)
		std::fill(&fftBuf[windowSize], &fftBuf[fftSize],
			  std::complex<float>(0, 0));

	return reinterpret_cast<std::complex<float>*>(fft.execute(fftBuf));
}

void computeReassignedTile(TFRMode mode,
			   const float *samples,
			   size_t sampleCount,
			   int windowSize, int fftSize,
			   const float *window,
			   int stride, int nCols,
			   float thresholdDB,
			   float *dest)
{
	if (nCols <= 0 || fftSize <= 0 || windowSize <= 0)
		return;

	size_t tileSize = (size_t)nCols * fftSize;
	size_t nSamples = sampleCount / 2;

	std::vector<float> accumPower(tileSize, 0.0f);

	std::vector<float> dhWin(windowSize);
	generateDerivativeWindow(window, windowSize, dhWin.data());

	std::vector<float> thWin;
	if (mode == TFRMode::Reassigned) {
		thWin.resize(windowSize);
		generateTimeRampedWindow(window, windowSize, thWin.data());
	}

	FFT fftH(fftSize);
	FFT fftDh(fftSize);
	std::unique_ptr<FFT> fftTh;
	if (mode == TFRMode::Reassigned)
		fftTh = std::make_unique<FFT>(fftSize);

	auto bufH = std::make_unique<std::complex<float>[]>(fftSize);
	auto bufDh = std::make_unique<std::complex<float>[]>(fftSize);
	std::unique_ptr<std::complex<float>[]> bufTh;
	if (mode == TFRMode::Reassigned)
		bufTh = std::make_unique<std::complex<float>[]>(fftSize);

	const float invN2 = (windowSize > 0)
		? 1.0f / ((float)windowSize * windowSize) : 1.0f;

	/* pass 1: find raw peak power (unnormalized) */
	float rawPeakPower = 0.0f;
	for (int col = 0; col < nCols; col++) {
		size_t center = (size_t)col * stride + windowSize / 2;
		auto *rH = computeColumn(fftH, samples, nSamples,
					 center, windowSize, fftSize,
					 window, bufH.get());
		for (int k = 0; k < fftSize; k++) {
			float p = rH[k].real() * rH[k].real() +
				  rH[k].imag() * rH[k].imag();
			if (p > rawPeakPower)
				rawPeakPower = p;
		}
	}

	/* threshold in raw power scale */
	float rawThresh = 0.0f;
	if (rawPeakPower > 0.0f) {
		float peakDB = fast_log2f_approx(rawPeakPower * invN2) * dBFS_SCALE;
		rawThresh = dBtoLinear(peakDB - thresholdDB) / invN2;
	}

	/* pass 2: reassign and accumulate
	 * invN cancels in ratios: X_Dh/X_h = rDh*conj(rH)/|rH|^2
	 * Each FFT object has its own buffer - no copies needed. */

	const float freqScale = (float)fftSize / TWO_PI_F;
	const float invStride = 1.0f / std::max(stride, 1);
	const bool doTimeReassign = (mode == TFRMode::Reassigned);

	for (int col = 0; col < nCols; col++) {
		size_t center = (size_t)col * stride + windowSize / 2;

		auto *rH = computeColumn(fftH, samples, nSamples,
					 center, windowSize, fftSize,
					 window, bufH.get());
		auto *rDh = computeColumn(fftDh, samples, nSamples,
					  center, windowSize, fftSize,
					  dhWin.data(), bufDh.get());
		std::complex<float> *rTh = nullptr;
		if (doTimeReassign)
			rTh = computeColumn(*fftTh, samples, nSamples,
					    center, windowSize, fftSize,
					    thWin.data(), bufTh.get());

		float colF = (float)col;
		/* floor below which division would produce inf and
		 * subsequent lrintf would be UB on inf/nan inputs */
		const float minPow = std::numeric_limits<float>::min();
		for (int k = 0; k < fftSize; k++) {
			float rawPow = rH[k].real() * rH[k].real() +
				       rH[k].imag() * rH[k].imag();
			if (rawPow < rawThresh || rawPow <= minPow)
				continue;

			std::complex<float> conjH = std::conj(rH[k]);
			float invPow = 1.0f / rawPow;

			/* k_hat = k - Im(X_Dh/X_h) * N/(2*pi) */
			std::complex<float> rd = rDh[k] * conjH * invPow;
			float f_hat = (float)k - freqScale * rd.imag();

			/* t_hat = t + Re(X_Th/X_h) / stride */
			float t_hat;
			if (doTimeReassign) {
				std::complex<float> rt = rTh[k] * conjH * invPow;
				t_hat = colF + rt.real() * invStride;
			} else {
				t_hat = colF;
			}

			int out_col = lrintf(t_hat);
			int out_bin = lrintf(f_hat);
			out_bin = ((out_bin % fftSize) + fftSize) % fftSize;

			if (out_col < 0 || out_col >= nCols)
				continue;

			accumPower[(size_t)out_col * fftSize + out_bin] +=
				rawPow * invN2;
		}
	}

	/* convert to dB with FFT-shift (DC to centre) */
	int half = fftSize / 2;
	for (int col = 0; col < nCols; col++) {
		float *outCol = dest + (size_t)col * fftSize;
		float *accCol = accumPower.data() + (size_t)col * fftSize;

		for (int i = 0; i < half; i++) {
			float p = accCol[half + i];
			outCol[i] = (p > 0.0f)
				? fast_log2f_approx(p) * dBFS_SCALE
				: FLOOR_DB;
		}
		for (int i = 0; i < half; i++) {
			float p = accCol[i];
			outCol[half + i] = (p > 0.0f)
				? fast_log2f_approx(p) * dBFS_SCALE
				: FLOOR_DB;
		}
	}
}

const char *tfrModeName(TFRMode mode)
{
	switch (mode) {
	case TFRMode::Standard:        return "Standard";
	case TFRMode::Reassigned:      return "Reassigned";
	case TFRMode::Synchrosqueezed: return "Synchrosqueezed";
	default:                       return "Unknown";
	}
}
