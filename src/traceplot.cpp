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

#include <QPixmapCache>
#include <QtConcurrent>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include "crashlog.h"
#include "samplesource.h"
#include "traceplot.h"

/* C++11 specifies std::complex<T> has the layout of T[2] */
static_assert(sizeof(std::complex<float>) == 2 * sizeof(float),
    "std::complex<float> must be layout-compatible with float[2]");
static_assert(alignof(std::complex<float>) == alignof(float[2]),
    "std::complex<float> must be alignment-compatible with float[2]");

TracePlot::TracePlot(std::shared_ptr<AbstractSampleSource> source) : Plot(source) {
    connect(this, &TracePlot::imageReady, this, &TracePlot::handleImage);
}

void TracePlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (sampleRange.length() == 0 || rect.width() <= 0) return;

    size_t samplesPerColumn = std::max((size_t)1, sampleRange.length() / rect.width());
    size_t samplesPerTile = (size_t)tileWidth * samplesPerColumn;
    if (samplesPerTile == 0) return;
    size_t tileID = sampleRange.minimum / samplesPerTile;
    size_t tileOffset = sampleRange.minimum % samplesPerTile;
    int xOffset = (int)(tileOffset / samplesPerColumn);

    // Paint first (possibly partial) tile
    painter.drawPixmap(
        QRect(rect.x(), rect.y(), tileWidth - xOffset, height()),
        getTile(tileID++, samplesPerTile),
        QRect(xOffset, 0, tileWidth - xOffset, height())
    );

    // Paint remaining tiles (use rect.x() + rect.width() to include last pixel)
    int xEnd = rect.x() + rect.width();
    for (int x = tileWidth - xOffset; x < xEnd; x += tileWidth) {
        painter.drawPixmap(
            QRect(x, rect.y(), tileWidth, height()),
            getTile(tileID++, samplesPerTile)
        );
    }
}

QPixmap TracePlot::getTile(size_t tileID, size_t sampleCount)
{
    QString key = QStringLiteral("tp_%1_%2_%3")
        .arg((quintptr)this, 0, 16)
        .arg(tileID)
        .arg(sampleCount);

    QPixmap pixmap;
    if (QPixmapCache::find(key, &pixmap))
        return pixmap;

    if (!tasks.contains(key)) {
        range_t<size_t> sampleRange{tileID * sampleCount, (tileID + 1) * sampleCount};
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        (void)QtConcurrent::run(&TracePlot::drawTile, this, key, QRect(0, 0, tileWidth, height()), sampleRange);
#else
        QtConcurrent::run(this, &TracePlot::drawTile, key, QRect(0, 0, tileWidth, height()), sampleRange);
#endif
        tasks.insert(key);
    }
    pixmap = QPixmap(tileWidth, height());
    pixmap.fill(Qt::transparent);
    return pixmap;
}

void TracePlot::drawTile(QString key, const QRect &rect, range_t<size_t> sampleRange)
{
    QImage image(rect.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto firstSample = sampleRange.minimum;
    auto length = sampleRange.length();

    // Is it a 2-channel (complex) trace?
    if (auto cplxSrc = dynamic_cast<SampleSource<std::complex<float>>*>(sampleSource.get())) {
        auto samples = cplxSrc->getSamples(firstSample, length);
        if (samples == nullptr)
            return;

        painter.setPen(Qt::red);
        plotTrace(painter, rect, reinterpret_cast<float*>(samples.get()), length, 2);
        painter.setPen(Qt::blue);
        plotTrace(painter, rect, reinterpret_cast<float*>(samples.get())+1, length, 2);

    // Otherwise is it single channel?
    } else if (auto floatSrc = dynamic_cast<SampleSource<float>*>(sampleSource.get())) {
        auto samples = floatSrc->getSamples(firstSample, length);
        if (samples == nullptr)
            return;

        painter.setPen(Qt::green);
        plotTrace(painter, rect, samples.get(), length, 1);
    } else {
        CrashLog::log(CrashLog::LOG_ERROR,
                       "TracePlot::drawTile: unsupported source type '%s' "
                       "(this=%p) -- tile skipped",
                       sampleSource ? sampleSource->sampleType().name()
                                    : "(null)",
                       (void *)this);
        return;
    }

    emit imageReady(key, image);
}

void TracePlot::handleImage(QString key, QImage image)
{
    auto pixmap = QPixmap::fromImage(image);
    QPixmapCache::insert(key, pixmap);
    tasks.remove(key);
    emit repaint();
}

/*
 * Plot a waveform trace using min/max decimation.
 *
 * For each pixel column, find the min and max sample values and draw
 * a vertical line between them. This preserves fast transitions
 * (OOK edges, FSK steps) that would be lost with naive point-to-point
 * drawing when multiple samples map to the same pixel.
 *
 * The path connects: last_max -> col_min -> col_max -> next_min -> ...
 * ensuring continuous lines with no gaps at tile boundaries.
 */
void TracePlot::plotTrace(QPainter &painter, const QRect &rect,
			  float *samples, size_t count, int step)
{
	if (count == 0 || rect.width() <= 0 || rect.height() <= 0)
		return;

	int w = rect.width();
	int h = rect.height();
	float halfH = h * 0.5f;
	size_t totalSamples = count / step; /* actual sample count */

	/* samples per pixel column */
	float samplesPerPixel = (float)totalSamples / w;

	if (samplesPerPixel <= 1.0f) {
		/*
		 * Zoomed in: fewer samples than pixels.
		 * Draw point-to-point (original behavior, no decimation).
		 */
		QPainterPath path;
		float xScale = (float)w / std::max(totalSamples, (size_t)1);
		for (size_t i = 0; i < totalSamples; i++) {
			float sample = samples[i * step];
			float x = i * xScale;
			float y = (1.0f - sample) * halfH;
			x = std::max(0.0f, std::min(x, (float)(w - 1))) + rect.x();
			y = std::max(0.0f, std::min(y, (float)(h - 1))) + rect.y();

			if (i == 0)
				path.moveTo(x, y);
			else
				path.lineTo(x, y);
		}
		painter.drawPath(path);
		return;
	}

	/*
	 * Zoomed out: multiple samples per pixel.
	 * Min/max decimation: for each pixel column, find the min and max
	 * sample value and draw a vertical segment. Connect segments with
	 * lines to the next column's values.
	 */
	QPainterPath path;
	bool first = true;

	for (int col = 0; col < w; col++) {
		/* sample range for this pixel column */
		size_t s0 = (size_t)(col * samplesPerPixel);
		size_t s1 = (size_t)((col + 1) * samplesPerPixel);
		if (s0 >= totalSamples) s0 = totalSamples - 1;
		if (s1 > totalSamples) s1 = totalSamples;
		if (s1 <= s0) s1 = s0 + 1;

		/* find min/max in this column's sample range */
		float vmin = std::numeric_limits<float>::max();
		float vmax = -std::numeric_limits<float>::max();
		for (size_t i = s0; i < s1 && i < totalSamples; i++) {
			float v = samples[i * step];
			if (v < vmin) vmin = v;
			if (v > vmax) vmax = v;
		}

		/* convert to pixel coordinates */
		float x = (float)col + rect.x();
		float yMin = std::max(0.0f, std::min((1.0f - vmax) * halfH, (float)(h - 1))) + rect.y();
		float yMax = std::max(0.0f, std::min((1.0f - vmin) * halfH, (float)(h - 1))) + rect.y();

		if (first) {
			path.moveTo(x, yMin);
			first = false;
		} else {
			/* connect to this column from previous max */
			path.lineTo(x, yMin);
		}

		/* vertical segment within column (min to max) */
		if (yMax != yMin)
			path.lineTo(x, yMax);

	}

	painter.drawPath(path);
}
