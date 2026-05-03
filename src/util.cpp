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

#include "util.h"
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>

/* All number<->text conversions pin to classic locale so '.' is the
 * decimal separator regardless of system locale. */

std::string formatSIValue(float value)
{
    std::map<int, std::string> prefixes = {
        {  9,   "G" },
        {  6,   "M" },
        {  3,   "k" },
        {  0,   ""  },
        { -3,   "m" },
        { -6,   "µ" },
        { -9,   "n" },
    };

    int power = 0;
    while (value < 1.0f && power > -9) {
        value *= 1e3f;
        power -= 3;
    }
    while (value >= 1e3f && power < 9) {
        value *= 1e-3f;
        power += 3;
    }
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << value << prefixes[power];
    return ss.str();
}

std::string formatSIValueSigned(double value, const char *unit)
{
    /* sub-1 prefixes (m/u/n) needed so us..ms times don't collapse to "0.000" */
    static const struct { double threshold; double divisor; const char *suffix; } table[] = {
        { 1e9,   1e9,   "G" },
        { 1e6,   1e6,   "M" },
        { 1e3,   1e3,   "k" },
        { 1.0,   1.0,   ""  },
        { 1e-3,  1e-3,  "m" },
        { 1e-6,  1e-6,  "u" },
        { 1e-9,  1e-9,  "n" },
        { 0,     1e-12, "p" },
    };

    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::fixed;

    if (value == 0.0) {
        ss << '0' << unit;
        return ss.str();
    }

    double av = (value < 0) ? -value : value;
    const char *suffix = "";
    double divisor = 1.0;

    for (auto &e : table) {
        if (av >= e.threshold) {
            divisor = e.divisor;
            suffix = e.suffix;
            break;
        }
    }

    double scaled = value / divisor;

    double as = (scaled < 0) ? -scaled : scaled;
    if (as >= 100.0)
        ss << std::setprecision(1);
    else if (as >= 10.0)
        ss << std::setprecision(2);
    else
        ss << std::setprecision(3);

    ss << scaled << suffix << unit;
    return ss.str();
}

bool parseSIValue(const std::string &str, double &result)
{
    if (str.empty())
        return false;

    /*
     * Parse via std::istringstream with the classic ("C") locale
     * imbued so the decimal separator is always '.'. This is
     * thread-safe, unlike setlocale() which is process-global.
     */
    std::istringstream iss(str);
    iss.imbue(std::locale::classic());

    double val;
    iss >> val;
    if (iss.fail())
        return false;

    /* skip whitespace, then check optional unit prefix */
    char c = '\0';
    while (iss.get(c)) {
        if (c != ' ' && c != '\t')
            break;
    }

    switch (c) {
    case 'G': case 'g': val *= 1e9;  break;
    case 'M':           val *= 1e6;  break;
    case 'K': case 'k': val *= 1e3;  break;
    case 'm':           val *= 1e-3; break;
    case 'u':           val *= 1e-6; break;
    case 'n':           val *= 1e-9; break;
    case '\0':          break;
    default:            break; /* ignore trailing unit text (Hz, Bd, etc.) */
    }

    result = val;
    return true;
}

template<> const char* getFileNameFilter<std::complex<float>>() { return "complex<float> file (*.fc32)"; };
template<> const char* getFileNameFilter<float>() { return "float file (*.f32)"; };
