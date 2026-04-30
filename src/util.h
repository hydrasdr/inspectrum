/*
 *  Copyright (C) 2016, Mike Walters <mike@flomp.net>
 *  Copyright (C) 2016, Jared Boone, ShareBrained Technology, Inc.
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

#pragma once
#include <algorithm>
#include <complex>
#include <map>
#include <sstream>
#include "fastmath.h"

/* Tau (2*pi) without depending on the non-standard M_PI macro,
 * which requires _USE_MATH_DEFINES on MSVC and is a POSIX-only
 * extension elsewhere. Defined to double precision. */
static const double Tau = 6.283185307179586476925286766559;

template <class T> const T& clamp (const T& value, const T& min, const T& max)
{
    return std::min(max, std::max(min, value));
}

template<class Iter>
struct iter_pair_range : std::pair<Iter,Iter> {
    iter_pair_range(std::pair<Iter,Iter> const& x) : std::pair<Iter,Iter>(x) { }
    Iter begin() const {
        return this->first;
    }
    Iter end()   const {
        return this->second;
    }
};

template<class Iter>
inline iter_pair_range<Iter> as_range(std::pair<Iter,Iter> const& x)
{
    return iter_pair_range<Iter>(x);
}

template<class T>
struct range_t {
    T minimum;
    T maximum;

    range_t() = default;
    range_t(const range_t<T>&) = default;

    range_t<T>& operator=(const range_t<T> &other) {
        minimum = other.minimum;
        maximum = other.maximum;
        return *this;
    }

    range_t<T>& operator=(const std::initializer_list<T> &other) {
        if (other.size() == 2) {
            minimum = *other.begin();
            maximum = *(other.begin() + 1);
        }
        return *this;
    }

    const T length() {
        return maximum - minimum;
    }

    const T& clip(const T& value) const {
        return clamp(value, minimum, maximum);
    }

    void reset_if_outside(T& value, const T& reset_value) const {
        if( (value < minimum ) ||
            (value > maximum ) ) {
            value = reset_value;
        }
    }

    bool below_range(const T& value) const {
        return value < minimum;
    }

    bool contains(const T& value) const {
        // TODO: Subtle gotcha here! Range test doesn't include maximum!
        return (value >= minimum) && (value < maximum);
    }

    bool out_of_range(const T& value) const {
        // TODO: Subtle gotcha here! Range test in contains() doesn't include maximum!
        return !contains(value);
    }
};

std::string formatSIValue(float value);
std::string formatSIValueSigned(double value, const char *unit = "");
bool parseSIValue(const std::string &str, double &result);

template<typename T> const char* getFileNameFilter();
