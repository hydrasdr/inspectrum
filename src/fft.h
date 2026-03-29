/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <fftw3.h>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <string>

class FFT
{
public:
	FFT(int size);
	~FFT();
	void process(void *dest, void *source);
	fftwf_complex* execute(const void *source);
	int getSize() {
		return fftSize;
	}

	/* wisdom + plan cache -- call once at startup */
	static void initWisdom();
	static void saveWisdom();
	static bool needsPreWarm();
	static void preWarm(std::function<void(int, int)> progress = nullptr);
	static void cleanup();

private:
	int fftSize;
	fftwf_complex *fftwIn = nullptr;
	fftwf_plan fftwPlan = nullptr;

	/* shared plan cache: size -> FFTW_MEASURE plan */
	static std::mutex cacheMutex;
	static std::unordered_map<int, fftwf_plan> planCache;
	static std::string wisdomPath;
	static bool wisdomDirty;

	static fftwf_plan getCachedPlan(int size, fftwf_complex *in);
};
