/*
 *  Copyright (C) 2017, Mike Walters <mike@flomp.net>
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

#include "phasedemod.h"

/* 1/pi as double constant, avoiding non-standard M_PI macro */
static constexpr double INV_PI = 0.31830988618379067153776752674503;

PhaseDemod::PhaseDemod(std::shared_ptr<SampleSource<std::complex<float>>> src) : SampleBuffer(src)
{

}

void PhaseDemod::work(void *input, void *output, int count, size_t)
{
    auto in = static_cast<std::complex<float>*>(input);
    auto out = static_cast<float*>(output);
    for (int i = 0; i < count; i++) {
        out[i] = std::arg(in[i]) * (float)INV_PI;
    }
}
