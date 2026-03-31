/*
 *  Copyright (C) 2016, Mike Walters <mike@flomp.net>
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

#include "traceplot.h"
#include "samplesource.h"

class ThresholdPlot : public TracePlot
{
	Q_OBJECT

public:
	ThresholdPlot(std::shared_ptr<AbstractSampleSource> source);
	void paintFront(QPainter &painter, QRect &rect,
			range_t<size_t> sampleRange) override;
	void setCursorInfo(bool enabled, range_t<size_t> selectedSamples,
			   int segments);
	void invalidateBitsCache() { bitsCacheDirty = true; }
	void setLsbFirst(bool lsb) { lsbFirst = lsb; bitsCacheDirty = true; }
	bool isLsbFirst() const { return lsbFirst; }

	const std::vector<int> &extractBits();
	QString getBinaryString();
	QString getHexString();
	QString getAsciiString();

private:
	bool cursorsActive = false;
	range_t<size_t> cursorSamples = {0, 0};
	int segmentCount = 1;
	bool lsbFirst = false;
	bool bitsCacheDirty = true;
	std::vector<int> cachedBits;
};
