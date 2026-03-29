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

#include <QPainter>
#include <QFontMetrics>
#include "thresholdplot.h"

ThresholdPlot::ThresholdPlot(std::shared_ptr<AbstractSampleSource> source)
	: TracePlot(source)
{
}

void ThresholdPlot::setCursorInfo(bool enabled,
				  range_t<size_t> selectedSamples,
				  int segments)
{
	if (cursorsActive != enabled ||
	    cursorSamples.minimum != selectedSamples.minimum ||
	    cursorSamples.maximum != selectedSamples.maximum ||
	    segmentCount != segments)
		bitsCacheDirty = true;

	cursorsActive = enabled;
	cursorSamples = selectedSamples;
	segmentCount = segments;
}

const std::vector<int> &ThresholdPlot::extractBits()
{
	if (!bitsCacheDirty)
		return cachedBits;

	cachedBits.clear();
	bitsCacheDirty = false;

	if (!cursorsActive || segmentCount <= 0 ||
	    cursorSamples.maximum <= cursorSamples.minimum)
		return cachedBits;

	auto floatSrc = std::dynamic_pointer_cast<SampleSource<float>>(
		sampleSource);
	if (!floatSrc)
		return cachedBits;

	auto samples = floatSrc->getSamples(cursorSamples.minimum,
					     cursorSamples.length());
	if (!samples)
		return cachedBits;

	float step = (float)cursorSamples.length() / segmentCount;

	for (int i = 0; i < segmentCount; i++) {
		size_t idx = (size_t)(step / 2 + i * step);
		if (idx < cursorSamples.length())
			cachedBits.push_back(samples[idx] > 0 ? 1 : 0);
	}

	return cachedBits;
}

QString ThresholdPlot::getBinaryString()
{
	auto bits = extractBits();
	QString result;

	for (int b : bits)
		result += QString::number(b);

	return result;
}

static int bitsToNibble(const std::vector<int> &bits, size_t offset,
		       size_t count, bool lsb)
{
	int nibble = 0;

	for (size_t j = 0; j < count && offset + j < bits.size(); j++) {
		if (lsb)
			nibble |= bits[offset + j] << j;
		else
			nibble |= bits[offset + j] << (3 - j);
	}
	return nibble;
}

static int bitsToByte(const std::vector<int> &bits, size_t offset, bool lsb)
{
	int byte = 0;

	for (int j = 0; j < 8 && offset + j < bits.size(); j++) {
		if (lsb)
			byte |= bits[offset + j] << j;
		else
			byte |= bits[offset + j] << (7 - j);
	}
	return byte;
}

QString ThresholdPlot::getHexString()
{
	auto bits = extractBits();
	QString result;

	for (size_t i = 0; i + 3 < bits.size(); i += 4)
		result += QString::number(bitsToNibble(bits, i, 4, lsbFirst),
					  16).toUpper();

	size_t remaining = bits.size() % 4;

	if (remaining > 0)
		result += QString::number(
			bitsToNibble(bits, bits.size() - remaining,
				     remaining, lsbFirst),
			16).toUpper();

	return result;
}

QString ThresholdPlot::getAsciiString()
{
	auto bits = extractBits();
	QString result;

	for (size_t i = 0; i + 7 < bits.size(); i += 8) {
		int byte = bitsToByte(bits, i, lsbFirst);

		if (byte >= 0x20 && byte <= 0x7E)
			result += QChar(byte);
		else
			result += '.';
	}

	return result;
}

void ThresholdPlot::paintFront(QPainter &painter, QRect &rect,
			       range_t<size_t> sampleRange)
{
	if (!cursorsActive || segmentCount <= 0 ||
	    sampleRange.length() == 0 ||
	    cursorSamples.maximum <= cursorSamples.minimum)
		return;

	auto bits = extractBits();

	if (bits.empty())
		return;

	painter.save();
	painter.setClipRect(rect);

	/* compute pixel positions of cursor range within the view */
	float pixelsPerSample = (float)rect.width() / sampleRange.length();
	float minX, maxX;

	if (cursorSamples.minimum >= sampleRange.minimum)
		minX = rect.x() + (cursorSamples.minimum - sampleRange.minimum) *
			pixelsPerSample;
	else
		minX = rect.x() - (sampleRange.minimum - cursorSamples.minimum) *
			pixelsPerSample;

	maxX = minX + cursorSamples.length() * pixelsPerSample;

	float segWidth = (maxX - minX) / segmentCount;

	/* each row gets its own optimal font size based on its
	 * column width: bin=1seg, hex=4seg, asc=8seg */
	static const QString binChars[2] = {
		QStringLiteral("0"), QStringLiteral("1")
	};
	static const QString hexChars[16] = {
		QStringLiteral("0"), QStringLiteral("1"),
		QStringLiteral("2"), QStringLiteral("3"),
		QStringLiteral("4"), QStringLiteral("5"),
		QStringLiteral("6"), QStringLiteral("7"),
		QStringLiteral("8"), QStringLiteral("9"),
		QStringLiteral("A"), QStringLiteral("B"),
		QStringLiteral("C"), QStringLiteral("D"),
		QStringLiteral("E"), QStringLiteral("F")
	};

	/* uniform base size, hex/asc get +1px when bin is hidden
	 * (at small zoom they have more room) */
	int baseSz = std::max(6, std::min((int)(segWidth * 0.55f), 14));
	int binFontSz = baseSz;
	int hexFontSz = (baseSz < 7) ? std::min(baseSz + 1, 14) : baseSz;
	int ascFontSz = (baseSz < 7) ? std::min(baseSz + 2, 14) : baseSz;

	QFont baseFont = painter.font();
	baseFont.setBold(true);

	/* test if each row's character fits AND segments are wide
	 * enough to be individually visible in the waveform */
	auto charFits = [&](int fontSize, float colWidth) -> bool {
		baseFont.setPixelSize(fontSize);
		int cw = QFontMetrics(baseFont).horizontalAdvance(QStringLiteral("0"));
		return colWidth >= cw + 1;
	};

	bool showBin = segWidth >= 4 && charFits(binFontSz, segWidth);
	bool showHex = segWidth >= 2 && charFits(hexFontSz, segWidth * 4);
	bool showAsc = segWidth >= 1 && charFits(ascFontSz, segWidth * 8);

	if (!showBin && !showHex && !showAsc) {
		painter.restore();
		return;
	}

	/* row positions -- use largest visible font for spacing */
	int maxFontSz = std::max({showBin ? binFontSz : 0,
				  showHex ? hexFontSz : 0,
				  showAsc ? ascFontSz : 0});
	baseFont.setPixelSize(maxFontSz);
	int rowHeight = QFontMetrics(baseFont).height() + 1;
	int binY = rect.y() + rect.height() * 0.52;
	int hexY = binY + rowHeight;
	int ascY = hexY + rowHeight;

	/* draw row labels */
	QFont labelFont = baseFont;
	labelFont.setPixelSize(std::max(6, maxFontSz - 2));
	labelFont.setBold(false);
	painter.setFont(labelFont);
	QFontMetrics lfm(labelFont);
	painter.setPen(QColor(200, 200, 200));
	if (showBin)
		painter.drawText(rect.x() + 2, binY + lfm.ascent(), "bin");
	if (showHex)
		painter.drawText(rect.x() + 2, hexY + lfm.ascent(), "hex");
	if (showAsc)
		painter.drawText(rect.x() + 2, ascY + lfm.ascent(), "asc");

	/* draw binary values */
	if (showBin) {
		baseFont.setPixelSize(binFontSz);
		painter.setFont(baseFont);
		QFontMetrics bfm(baseFont);
		int bAscent = bfm.ascent();
		int bw0 = bfm.horizontalAdvance(binChars[0]);
		int bw1 = bfm.horizontalAdvance(binChars[1]);

		painter.setPen(Qt::white);
		for (size_t i = 0; i < bits.size(); i++) {
			float x = minX + i * segWidth;
			if (x + segWidth < rect.x() || x > rect.right())
				continue;
			int b = bits[i] & 1;
			int w = b ? bw1 : bw0;
			int textX = (int)(x + (segWidth - w) * 0.5f);
			painter.drawText(textX, binY + bAscent, binChars[b]);
		}
	}

	/* draw hex values every 4 bits */
	if (showHex) {
		baseFont.setPixelSize(hexFontSz);
		painter.setFont(baseFont);
		QFontMetrics hfm(baseFont);
		int hAscent = hfm.ascent();
		int hCharW = hfm.horizontalAdvance(hexChars[0]);

		painter.setPen(QColor(255, 200, 50));
		for (size_t i = 0; i < bits.size(); i += 4) {
			size_t count = std::min((size_t)4, bits.size() - i);
			int nibble = bitsToNibble(bits, i, count, lsbFirst);
			float x = minX + i * segWidth;
			float hexWidth = count * segWidth;
			if (x + hexWidth < rect.x() || x > rect.right())
				continue;
			int textX = (int)(x + (hexWidth - hCharW) * 0.5f);
			painter.drawText(textX, hexY + hAscent,
					 hexChars[nibble & 0xF]);
		}
	}

	/* draw ASCII values every 8 bits */
	if (showAsc) {
		baseFont.setPixelSize(ascFontSz);
		painter.setFont(baseFont);
		QFontMetrics afm(baseFont);
		int aAscent = afm.ascent();
		int dotW = afm.horizontalAdvance(QStringLiteral("."));

		painter.setPen(QColor(100, 220, 255));
		for (size_t i = 0; i + 7 < bits.size(); i += 8) {
			int byte = bitsToByte(bits, i, lsbFirst);
			float x = minX + i * segWidth;
			float ascWidth = 8 * segWidth;
			if (x + ascWidth < rect.x() || x > rect.right())
				continue;
			if (byte >= 0x20 && byte <= 0x7E) {
				QChar ch(byte);
				int cw = afm.horizontalAdvance(ch);
				int textX = (int)(x + (ascWidth - cw) * 0.5f);
				painter.drawText(textX, ascY + aAscent, ch);
			} else {
				int textX = (int)(x + (ascWidth - dotW) * 0.5f);
				painter.drawText(textX, ascY + aAscent,
						 QStringLiteral("."));
			}
		}
	}

	painter.restore();
}
