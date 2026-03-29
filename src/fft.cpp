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

#include "fft.h"
#include <cstring>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

std::mutex FFT::cacheMutex;
std::unordered_map<int, fftwf_plan> FFT::planCache;
std::string FFT::wisdomPath;
bool FFT::wisdomDirty = false;

void FFT::initWisdom()
{
	/* try exe directory first (portable/local install),
	 * fall back to user data dir (system-wide install on Linux
	 * where /usr/bin is read-only) */
	QString exeDir = QCoreApplication::applicationDirPath();
	QString exePath = exeDir + "/fftw_wisdom.dat";

	if (QFileInfo(exeDir).isWritable()) {
		wisdomPath = exePath.toStdString();
	} else {
		QString userDir = QStandardPaths::writableLocation(
			QStandardPaths::AppLocalDataLocation);
		QDir().mkpath(userDir);
		wisdomPath = (userDir + "/fftw_wisdom.dat").toStdString();
	}

	if (fftwf_import_wisdom_from_filename(wisdomPath.c_str()))
		qDebug() << "FFTW: loaded wisdom from" << wisdomPath.c_str();
}

void FFT::saveWisdom()
{
	if (!wisdomDirty || wisdomPath.empty())
		return;
	if (fftwf_export_wisdom_to_filename(wisdomPath.c_str()))
		qDebug() << "FFTW: saved wisdom to" << wisdomPath.c_str();
	wisdomDirty = false;
}

bool FFT::needsPreWarm()
{
	for (int exp = 2; exp <= 17; exp++) {
		if (!planCache.count(1 << exp))
			return true;
	}
	return false;
}

void FFT::preWarm(std::function<void(int, int)> progress)
{
	/* pre-create plans for all power-of-2 sizes used by the app:
	 * window 2^2..2^14, zero-pad 1x..8x -> FFT sizes 4..131072 */
	int total = 17 - 2 + 1;  /* exponents 2..17 */
	int step = 0;

	for (int exp = 2; exp <= 17; exp++) {
		int size = 1 << exp;

		if (progress)
			progress(step, total);
		step++;

		if (planCache.count(size))
			continue;

		fftwf_complex *tmp = (fftwf_complex *)fftwf_malloc(
			sizeof(fftwf_complex) * size);
		if (!tmp)
			continue;

		fftwf_plan p = fftwf_plan_dft_1d(size, tmp, tmp,
			FFTW_FORWARD, FFTW_MEASURE);
		if (p) {
			planCache[size] = p;
			wisdomDirty = true;
		}

		fftwf_free(tmp);
	}

	if (progress)
		progress(total, total);

	saveWisdom();
	qDebug() << "FFTW: pre-warmed" << planCache.size() << "plans";
}

void FFT::cleanup()
{
	std::lock_guard<std::mutex> lock(cacheMutex);
	for (auto &kv : planCache)
		fftwf_destroy_plan(kv.second);
	planCache.clear();
	fftwf_cleanup();
}

fftwf_plan FFT::getCachedPlan(int size, fftwf_complex *in)
{
	std::lock_guard<std::mutex> lock(cacheMutex);

	auto it = planCache.find(size);
	if (it != planCache.end())
		return it->second;

	fftwf_plan p = fftwf_plan_dft_1d(size, in, in,
		FFTW_FORWARD, FFTW_MEASURE);
	if (p) {
		planCache[size] = p;
		wisdomDirty = true;
	}
	return p;
}

FFT::FFT(int size)
{
	fftSize = size;
	fftwIn = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
	if (fftwIn)
		fftwPlan = getCachedPlan(fftSize, fftwIn);
}

FFT::~FFT()
{
	/* plans are shared in planCache -- destroyed by cleanup() at exit */
	if (fftwIn)
		fftwf_free(fftwIn);
}

void FFT::process(void *dest, void *source)
{
	if (!fftwPlan || !fftwIn) {
		memset(dest, 0, fftSize * sizeof(fftwf_complex));
		return;
	}
	memcpy(fftwIn, source, fftSize * sizeof(fftwf_complex));
	fftwf_execute_dft(fftwPlan, fftwIn, fftwIn);
	memcpy(dest, fftwIn, fftSize * sizeof(fftwf_complex));
}

fftwf_complex* FFT::execute(const void *source)
{
	if (!fftwPlan || !fftwIn)
		return nullptr;
	memcpy(fftwIn, source, fftSize * sizeof(fftwf_complex));
	fftwf_execute_dft(fftwPlan, fftwIn, fftwIn);
	return fftwIn;
}
