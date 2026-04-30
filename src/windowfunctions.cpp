/*
 *  Copyright (C) 2026, Benjamin Vernoux <bvernoux@hydrasdr.com>
 *
 *  Window function library for inspectrum ng.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "windowfunctions.h"
#include "util.h"
#include <cmath>

/*
 * Modified Bessel function of the first kind, order 0.
 * Series expansion: I0(x) = sum_{k=0}^inf ((x/2)^k / k!)^2
 * Converges rapidly; 25 terms gives >15 digits of precision for |x| <= 30.
 */
static double bessel_i0(double x)
{
	double sum = 1.0;
	double term = 1.0;
	double x_half = x * 0.5;

	for (int k = 1; k <= 25; k++) {
		term *= (x_half / k);
		sum += term * term;
	}
	return sum;
}

void generateWindow(WindowType type, int size, float *dest, float beta)
{
	if (size <= 0)
		return;

	int N = size - 1;
	if (N < 1)
		N = 1;

	double invN = 1.0 / N;

	switch (type) {
	case WindowType::Rectangular:
		for (int i = 0; i < size; i++)
			dest[i] = 1.0f;
		break;

	case WindowType::Hann:
		for (int i = 0; i < size; i++)
			dest[i] = (float)(0.5 * (1.0 - cos(Tau * i * invN)));
		break;

	case WindowType::Hamming:
		for (int i = 0; i < size; i++)
			dest[i] = (float)(0.54 - 0.46 * cos(Tau * i * invN));
		break;

	case WindowType::BlackmanHarris:
		/* 4-term Blackman-Harris: -92 dB sidelobes */
		for (int i = 0; i < size; i++) {
			double x = Tau * i * invN;
			dest[i] = (float)(0.35875
					  - 0.48829 * cos(x)
					  + 0.14128 * cos(2.0 * x)
					  - 0.01168 * cos(3.0 * x));
		}
		break;

	case WindowType::Kaiser: {
		double denom = bessel_i0(beta);
		if (denom == 0.0) denom = 1.0; /* beta == 0 edge: I0(0)=1, but guard anyway */
		double halfN = N * 0.5;
		for (int i = 0; i < size; i++) {
			double t = (i - halfN) / halfN;
			/* clamp to >=0 to absorb rounding error at edges
			 * (mathematically t in [-1,1], so 1-t*t >= 0) */
			double radicand = 1.0 - t * t;
			if (radicand < 0.0) radicand = 0.0;
			double arg = beta * sqrt(radicand);
			dest[i] = (float)(bessel_i0(arg) / denom);
		}
		break;
	}

	case WindowType::FlatTop:
		/*
		 * Flat-top window (ISO 18431-2 / HFT90D).
		 * Excellent amplitude accuracy, wide mainlobe.
		 * Coefficients normalized so peak = 1.0.
		 */
		for (int i = 0; i < size; i++) {
			double x = Tau * i * invN;
			double w = 1.0
				   - 1.93 * cos(x)
				   + 1.29 * cos(2.0 * x)
				   - 0.388 * cos(3.0 * x)
				   + 0.0322 * cos(4.0 * x);
			/* 0.9644 = peak of unnormalized flat-top window */
			dest[i] = (float)(w / 0.9644);
		}
		break;

	default:
		/* fallback to Hann */
		for (int i = 0; i < size; i++)
			dest[i] = (float)(0.5 * (1.0 - cos(Tau * i * invN)));
		break;
	}
}

const char *windowTypeName(WindowType type)
{
	switch (type) {
	case WindowType::Hann:           return "Hann";
	case WindowType::Hamming:        return "Hamming";
	case WindowType::BlackmanHarris: return "Blackman-Harris";
	case WindowType::Kaiser:         return "Kaiser";
	case WindowType::FlatTop:        return "Flat-top";
	case WindowType::Rectangular:    return "Rectangular";
	default:                         return "Unknown";
	}
}
