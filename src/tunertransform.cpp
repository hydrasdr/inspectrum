/*
 *  Copyright (C) 2016, Mike Walters <mike@flomp.net>
 *  Copyright (C) 2026, Benjamin Vernoux <bvernoux@hydrasdr.com>
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

#include "tunertransform.h"
#include <cmath>
#include <liquid/liquid.h>
#include "util.h"

TunerTransform::TunerTransform(std::shared_ptr<SampleSource<std::complex<float>>> src)
    : SampleBuffer(src), frequency(0), bandwidth(1.), taps{1.0f}
{
    nco = nco_crcf_create(LIQUID_NCO);
    rebuildFilter();
}

TunerTransform::~TunerTransform()
{
    if (nco)
        nco_crcf_destroy(nco);
    if (filter)
        firfilt_crcf_destroy(filter);
}

void TunerTransform::rebuildFilter()
{
    if (filter)
        firfilt_crcf_destroy(filter);
    /* liquid-dsp takes 'unsigned int' for filter length; FIR taps
     * count is well below UINT_MAX */
    filter = firfilt_crcf_create(taps.data(), (unsigned int)taps.size());
    filterDirty = false;
}

void TunerTransform::work(void *input, void *output, int count, size_t sampleid)
{
    /*
     * Snapshot parameters under lock, then release before heavy
     * computation.  The setters only touch frequency/taps/gain/
     * filterDirty, so we copy what we need and let the UI thread
     * proceed while we compute.
     */
    float freq;
    float g;
    {
        QMutexLocker lock(&paramMutex);
        freq = frequency;
        g = gain;

        /* rebuild filter if taps changed (must be under lock
         * because rebuildFilter reads taps vector) */
        if (filterDirty)
            rebuildFilter();
    }

    auto out = static_cast<std::complex<float>*>(output);

    /* grow mix buffer if needed (no realloc if already large enough) */
    if ((int)mixBuf.size() < count)
        mixBuf.resize(count);

    /* mix down using pre-allocated NCO. Compute the modulo in double
     * precision: freq * sampleid in float exhausts mantissa for files
     * larger than ~16M samples, causing visible NCO phase drift. */
    double phase = fmod((double)freq * (double)sampleid, Tau);
    nco_crcf_set_phase(nco, (float)phase);
    nco_crcf_set_frequency(nco, freq);
    nco_crcf_mix_block_down(nco,
                            static_cast<std::complex<float>*>(input),
                            mixBuf.data(),
                            count);

    /* reset filter state (no lock needed -- filter object is only
     * used by worker threads serialised via SampleBuffer::mutex) */
    firfilt_crcf_reset(filter);

    /* filter */
    for (int i = 0; i < count; i++) {
        firfilt_crcf_push(filter, mixBuf[i]);
        firfilt_crcf_execute(filter, &out[i]);
    }

    /* apply gain */
    if (g != 1.0f) {
        for (int i = 0; i < count; i++)
            out[i] *= g;
    }
}

void TunerTransform::setFrequency(float newFreq)
{
    QMutexLocker lock(&paramMutex);
    frequency = newFreq;
}

void TunerTransform::setTaps(std::vector<float> newTaps)
{
    QMutexLocker lock(&paramMutex);
    taps = std::move(newTaps);
    filterDirty = true;
}

void TunerTransform::setGain(float g)
{
    QMutexLocker lock(&paramMutex);
    this->gain = g;
}

float TunerTransform::relativeBandwidth() {
    QMutexLocker lock(&paramMutex);
    return bandwidth;
}

void TunerTransform::setRelativeBandwith(float newBw)
{
    QMutexLocker lock(&paramMutex);
    bandwidth = newBw;
}

