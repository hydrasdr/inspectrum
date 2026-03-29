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

#include "spectrogramplot.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmapCache>
#include <QRect>
#include <liquid/liquid.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include "util.h"

/*
 * Fast IEEE 754 approximations for log2 and exp2.
 *
 * IEEE 754 float bit layout: [sign:1][exponent:8][mantissa:23]
 * For a positive normal float x = 2^(e-127) * (1 + m/2^23), so:
 *   bits(x) = (e << 23) | m  ~=  2^23 * (log2(x) + 127)
 *
 * Rearranging: log2(x) ~= (bits(x) - 127*2^23) / 2^23
 *
 * Max absolute error: ~0.086 in log2 units = ~0.29 dB.
 * A 256-color spectrogram over 100 dB has ~0.39 dB per color step,
 * so the error is sub-pixel and visually identical to standard log2f.
 *
 * Edge cases (all safe for spectrogram use):
 *   power = 0    -> ~-422 dB (standard: -inf) -> clamped to darkest color
 *   power = +inf -> ~+425 dB (standard: +inf) -> clamped to brightest color
 *   denormals    -> very negative dB, below display range -> darkest color
 */
static inline float fast_log2f_approx(float x)
{
    int32_t i;
    memcpy(&i, &x, sizeof(i));
    return (float)(i - 0x3F800000) * 1.1920928955078125e-7f; /* 1/(1<<23) */
}

static inline float fast_exp2f_approx(float x)
{
    int32_t i = (int32_t)(x * 8388608.0f) + 0x3F800000; /* x*(1<<23) + 127*(1<<23) */
    float r;
    memcpy(&r, &i, sizeof(r));
    return r;
}

/* ---- Profiling instrumentation ---- */
/* set to 1 to enable per-tile profiling output via qDebug */
#define INSPECTRUM_PROFILE 0

#if INSPECTRUM_PROFILE
#include <QDebug>

struct TileProfile {
    qint64 getSamples_ns = 0;
    qint64 window_ns     = 0;
    qint64 fft_ns        = 0;
    qint64 magnitude_ns  = 0;
    qint64 tile_total_ns = 0;
    qint64 pixmap_ns     = 0;
    qint64 enhanced_ns   = 0;
    int    lines         = 0;
    int    fftSize       = 0;
    int    windowSize    = 0;

    void report(const char *tag) {
        if (lines == 0) return;
        qDebug("[PROFILE %s] fft=%d win=%d lines=%d | "
               "getSamples=%lld window=%lld fft=%lld mag=%lld | "
               "tile=%lld enhanced=%lld pixmap=%lld us",
               tag, fftSize, windowSize, lines,
               getSamples_ns/1000, window_ns/1000,
               fft_ns/1000, magnitude_ns/1000,
               tile_total_ns/1000, enhanced_ns/1000, pixmap_ns/1000);
    }
};

/* per-tile accumulator, reset at start of each getFFTTile */
static thread_local TileProfile g_prof;
#endif


SpectrogramPlot::SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>> src) : Plot(src), inputSource(src), fftSize(512), windowSize(512), tuner(fftSize, this)
{
    pixmapCache.setMaxCost(128 * 1024);   /* ~128 MB of pixmaps */
    fftCache.setMaxCost(128 * 1024);      /* ~128 MB of FFT data */
    enhancedCache.setMaxCost(128 * 1024); /* ~128 MB of enhanced data */

    setFFTSize(windowSize);
    zoomLevel = 1;
    powerMax = 0.0f;
    powerMin = -50.0f;
    sampleRate = 0;
    frequencyScaleEnabled = false;
    sigmfAnnotationsEnabled = true;

    for (int i = 0; i < 256; i++) {
        float p = (float)i / 256;
        colormap[i] = QColor::fromHsvF(p * 0.83f, 1.0, 1.0 - p).rgba();
    }

    tunerTransform = std::make_shared<TunerTransform>(src);
    connect(&tuner, &Tuner::tunerMoved, this, &SpectrogramPlot::tunerMoved);

    tunerUpdateTimer.setSingleShot(true);
    connect(&tunerUpdateTimer, &QTimer::timeout,
            this, &SpectrogramPlot::tunerFullUpdate);

    zoomRenderTimer.setSingleShot(true);
    connect(&zoomRenderTimer, &QTimer::timeout,
            this, &SpectrogramPlot::zoomRenderNow);
}

void SpectrogramPlot::invalidateEvent()
{
    // HACK: this makes sure we update the height for real signals (as InputSource is passed here before the file is opened)
    // Force re-run by passing windowSize (not fftSize which includes zeroPad)
    int savedWin = windowSize;
    windowSize = 0;  /* force setFFTSize to run */
    setFFTSize(savedWin);

    pixmapCache.clear();
    enhancedCache.clear();
    fftCache.clear();
    emit repaint();
}

void SpectrogramPlot::paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
#if INSPECTRUM_PROFILE
    QElapsedTimer pfTimer; pfTimer.start();
#endif
    if (tunerEnabled() && tunerVisible) {
        if (maskOutOfBand) {
            /* draw tuner in cropped coordinates */
            int cropCentre = rect.height() / 2;
            int dev = tuner.deviation();

            painter.save();

            /* tuner band highlight */
            int tunerTop = rect.top() + cropCentre - dev;
            int tunerBot = rect.top() + cropCentre + dev;
            painter.fillRect(
                QRect(rect.left(), tunerTop, rect.width(), dev * 2),
                QBrush(QColor(255, 255, 255, 50)));

            /* tuner edges */
            painter.setPen(QPen(Qt::white, 1, Qt::SolidLine));
            painter.drawLine(rect.left(), tunerTop, rect.right(), tunerTop);
            painter.drawLine(rect.left(), tunerBot, rect.right(), tunerBot);

            /* center frequency */
            painter.setPen(QPen(Qt::red, 1, Qt::SolidLine));
            painter.drawLine(rect.left(), rect.top() + cropCentre,
                             rect.right(), rect.top() + cropCentre);

            painter.restore();
        } else {
            tuner.paintFront(painter, rect, sampleRange);
        }
    }

    if (frequencyScaleEnabled)
        paintFrequencyScale(painter, rect);

#if INSPECTRUM_PROFILE
    qint64 pf_tuner_scale = pfTimer.nsecsElapsed(); pfTimer.restart();
#endif

    if (sigmfAnnotationsEnabled)
        paintAnnotations(painter, rect, sampleRange);

#if INSPECTRUM_PROFILE
    qint64 pf_annot = pfTimer.nsecsElapsed();
    qDebug("[PROFILE FRONT] tuner+scale=%lld annotations=%lld total=%lld us",
           pf_tuner_scale/1000, pf_annot/1000,
           (pf_tuner_scale+pf_annot)/1000);
#endif
}

void SpectrogramPlot::paintFrequencyScale(QPainter &painter, QRect &rect)
{
    if (sampleRate == 0) {
        return;
    }

    if (sampleRate / 2 > UINT64_MAX) {
        return;
    }

    int fullHeight = inputSource->realSignal() ? fftSize / 2 : fftSize;
    if (fullHeight <= 0)
        return;

    /* Hz per FFT bin (1 bin = 1 pixel in uncropped mode) */
    double hzPerBin = (double)sampleRate / fullHeight;

    double hzPerPixel = hzPerBin;

    if (maskOutOfBand && tunerEnabled()) {
        /* cropped: rect.height() pixels show height() bins */
        if (rect.height() > 0 && height() > 0)
            hzPerPixel = hzPerBin * height() / rect.height();
    }

    int tickHeight = 50;

    uint64_t bwPerTick = 10 * pow(10, floor(log(hzPerPixel * tickHeight) / log(10)));

    if (bwPerTick < 1)
        return;

    painter.save();

    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());

    /* view center in pixels */
    int viewCentrePx = rect.y() + rect.height() / 2;

    uint64_t tick = 0;

    while (tick <= sampleRate / 2) {
        /* positive freq tick: above center (viewCentreHz + tick)
         * negative freq tick: below center (viewCentreHz - tick) */
        int tickpy = viewCentrePx - (int)(tick / hzPerPixel);
        int tickny = viewCentrePx + (int)(tick / hzPerPixel);

        bool pyVis = tickpy >= rect.top() && tickpy <= rect.bottom();
        bool nyVis = tickny >= rect.top() && tickny <= rect.bottom();

        /* stop if both ticks are outside the view */
        if (!pyVis && !nyVis && tick > 0)
            break;

        if (!inputSource->realSignal() && nyVis)
            painter.drawLine(0, tickny, 30, tickny);
        if (pyVis)
            painter.drawLine(0, tickpy, 30, tickpy);

        if (tick != 0) {
            char buf[128];

            if (bwPerTick % 1000000000 == 0)
                snprintf(buf, sizeof(buf), "-%llu GHz", (unsigned long long)(tick / 1000000000));
            else if (bwPerTick % 1000000 == 0)
                snprintf(buf, sizeof(buf), "-%llu MHz", (unsigned long long)(tick / 1000000));
            else if (bwPerTick % 1000 == 0)
                snprintf(buf, sizeof(buf), "-%llu kHz", (unsigned long long)(tick / 1000));
            else
                snprintf(buf, sizeof(buf), "-%llu Hz", (unsigned long long)tick);

            if (!inputSource->realSignal() && nyVis)
                painter.drawText(5, tickny - 5, buf);

            buf[0] = ' ';
            if (pyVis)
                painter.drawText(5, tickpy + 15, buf);
        }

        tick += bwPerTick;
    }

    // Draw small ticks
    bwPerTick /= 10;

    if (bwPerTick >= 1) {
        tick = 0;
        while (tick <= sampleRate / 2) {
            int tickpy = viewCentrePx - (int)(tick / hzPerPixel);
            int tickny = viewCentrePx + (int)(tick / hzPerPixel);

            bool pyVis = tickpy >= rect.top() && tickpy <= rect.bottom();
            bool nyVis = tickny >= rect.top() && tickny <= rect.bottom();

            if (!pyVis && !nyVis && tick > 0)
                break;

            if (!inputSource->realSignal() && nyVis)
                painter.drawLine(0, tickny, 3, tickny);
            if (pyVis)
                painter.drawLine(0, tickpy, 3, tickpy);

            tick += bwPerTick;
        }
    }
    painter.restore();
}

void SpectrogramPlot::paintAnnotations(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    // Pixel (from the top) at which 0 Hz sits
    int zero = rect.y() + rect.height() / 2;

    painter.save();
    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());

    visibleAnnotationLocations.clear();

    for (size_t i = 0; i < inputSource->annotationList.size(); i++) {
        Annotation a = inputSource->annotationList.at(i);

        size_t labelLength = fm.boundingRect(a.label).width() * getStride();

        // Check if:
        //  (1) End of annotation (might be maximum, or end of label text) is still visible in time
        //  (2) Part of the annotation is already visible in time
        //
        // Currently there is no check if the annotation is visible in frequency. This is a
        // possible performance improvement
        //
        size_t start = a.sampleRange.minimum;
        size_t end = std::max(a.sampleRange.minimum + labelLength, a.sampleRange.maximum);

        if(start <= sampleRange.maximum && end >= sampleRange.minimum) {

            double frequency = a.frequencyRange.maximum - inputSource->getFrequency();
            int x = (a.sampleRange.minimum - sampleRange.minimum) / getStride();
            int y = zero - frequency / sampleRate * rect.height();
            int height = (a.frequencyRange.maximum - a.frequencyRange.minimum) / sampleRate * rect.height();
            int width = (a.sampleRange.maximum - a.sampleRange.minimum) / getStride();

            // Draw the label 2 pixels above the box
            painter.drawText(x, y - 2, a.label);
            painter.drawRect(x, y, width, height);

            visibleAnnotationLocations.emplace_back(a, x, y, width, height);
        }
    }

    painter.restore();
}

QString *SpectrogramPlot::mouseAnnotationComment(const QMouseEvent *event) {
    auto pos = event->pos();
    int mouse_x = pos.x();
    int mouse_y = pos.y();

    for (auto& a : visibleAnnotationLocations) {
        if (!a.annotation.comment.isEmpty() && a.isInside(mouse_x, mouse_y)) {
            return &a.annotation.comment;
        }
    }
    return nullptr;
}

void SpectrogramPlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (!inputSource || inputSource->count() == 0)
        return;

    QElapsedTimer renderTimer;
    renderTimer.start();

    int stride = getStride();
    int lpt = linesPerTile();
    if (stride <= 0 || lpt <= 0)
        return;

    size_t sampleOffset = sampleRange.minimum % ((size_t)stride * lpt);
    size_t tileID = sampleRange.minimum - sampleOffset;
    int xoffset = sampleOffset / stride;

    /*
     * Compute the vertical crop.
     * - "Crop to tuner": show exactly the tuner bandwidth
     * - Otherwise: show fftSize/yZoomLevel bins centered on tuner
     */
    int visibleBins;
    int yCenter = tuner.centre();

    if (maskOutOfBand && tunerEnabled()) {
        /* crop to tuner bandwidth exactly */
        visibleBins = tuner.deviation() * 2;
        if (visibleBins < 2)
            visibleBins = 2;
        if (visibleBins > fftSize)
            visibleBins = fftSize;
    } else {
        visibleBins = fftSize / yZoomLevel;
    }

    /* clamp so the crop window stays within [0, fftSize) */
    int yTop = yCenter - visibleBins / 2;
    if (yTop < 0)
        yTop = 0;
    if (yTop + visibleBins > fftSize)
        yTop = fftSize - visibleBins;

    // Paint first (possibly partial) tile
    painter.drawPixmap(
        QRect(rect.left(), rect.y(), lpt - xoffset, height()),
        *getPixmapTile(tileID),
        QRect(xoffset, yTop, lpt - xoffset, visibleBins));
    tileID += (size_t)stride * lpt;

    // Paint remaining tiles (use rect.x() + rect.width() to include last pixel)
    int xEnd = rect.x() + rect.width();
    for (int x = lpt - xoffset; x < xEnd; x += lpt) {
        painter.drawPixmap(
            QRect(x, rect.y(), lpt, height()),
            *getPixmapTile(tileID),
            QRect(0, yTop, lpt, visibleBins));
        tileID += (size_t)stride * lpt;
    }

    /* measure render time for adaptive zoom deferral */
    lastRenderMs = (int)renderTimer.elapsed();
    emit renderTimeChanged(lastRenderMs);
}

QPixmap* SpectrogramPlot::getPixmapTile(size_t tile)
{
    QPixmap *obj = pixmapCache.object(TileCacheKey(fftSize, zoomLevel, tile));
    if (obj != nullptr)
        return obj;
#if INSPECTRUM_PROFILE
    QElapsedTimer pmTimer; pmTimer.start();
#endif

    /* during rapid zoom, skip expensive tile computation --
     * return empty tile, real tiles render when zoom settles */
    if (zoomDeferred) {
        static QPixmap deferredPixmap(1, 1);
        deferredPixmap.fill(Qt::black);
        return &deferredPixmap;
    }

    float *fftTile = getEnhancedTile(tile);
    int lpt = linesPerTile();
    obj = new QPixmap(lpt, fftSize);
    QImage image(lpt, fftSize, QImage::Format_RGB32);
#if INSPECTRUM_PROFILE
    qint64 pm_alloc = pmTimer.nsecsElapsed(); pmTimer.restart();
#endif
    float pRange = powerRange;  /* use precomputed value */

    /* Outer-x / inner-y loop order: fftTile reads are sequential
     * (column-major data), giving 4-7x speedup at fftSize >= 512
     * vs the strided outer-y order. Precompute scanLine pointers
     * to avoid per-row QImage::scanLine() calls in the inner loop. */
    if ((int)scanLinePtrs.size() < fftSize)
        scanLinePtrs.resize(fftSize);
    for (int y = 0; y < fftSize; y++)
        scanLinePtrs[y] = (QRgb*)image.scanLine(fftSize - y - 1);

    for (int x = 0; x < lpt; x++) {
        const float *col = fftTile + (size_t)x * fftSize;
        for (int y = 0; y < fftSize; y++) {
            float normPower = clamp(
                (col[y] - powerMax) * pRange,
                0.0f, 1.0f);
            scanLinePtrs[y][x] = colormap[(uint8_t)(normPower * 255.0f)];
        }
    }
#if INSPECTRUM_PROFILE
    qint64 pm_fill = pmTimer.nsecsElapsed(); pmTimer.restart();
#endif
    obj->convertFromImage(image);
#if INSPECTRUM_PROFILE
    qint64 pm_convert = pmTimer.nsecsElapsed();
#endif
    int pmCostKB = (int)((size_t)lpt * fftSize * 4 / 1024);
    pixmapCache.insert(TileCacheKey(fftSize, zoomLevel, tile), obj,
                       std::max(pmCostKB, 1));
#if INSPECTRUM_PROFILE
    qDebug("[PROFILE PIXMAP] fft=%d lpt=%d | alloc=%lld fill=%lld convert=%lld total=%lld us",
           fftSize, lpt, pm_alloc/1000, pm_fill/1000, pm_convert/1000,
           (pm_alloc+pm_fill+pm_convert)/1000);
#endif
    return obj;
}

float* SpectrogramPlot::getFFTTile(size_t tile)
{
    std::vector<float>* obj = fftCache.object(TileCacheKey(fftSize, zoomLevel, tile));
    if (obj != nullptr)
        return obj->data();

#if INSPECTRUM_PROFILE
    QElapsedTimer tileTimer; tileTimer.start();
    g_prof = TileProfile();  /* reset per-line accumulators */
    g_prof.fftSize = fftSize;
    g_prof.windowSize = windowSize;
#endif

    int lpt = linesPerTile();
    size_t tileDataSize = (size_t)lpt * fftSize;

    /* compute into reusable scratch buffer (avoids zeroing a fresh allocation) */
    if (tileWorkBuf.size() < tileDataSize)
        tileWorkBuf.resize(tileDataSize);
    float *ptr = tileWorkBuf.data();
    size_t sample = tile;
    for (int i = 0; i < lpt; i++) {
        getLine(ptr, sample);
        sample += getStride();
        ptr += fftSize;
    }

    /* copy result into a cache-owned allocation */
    auto *destStorage = new std::vector<float>(tileWorkBuf.begin(),
                                               tileWorkBuf.begin() + tileDataSize);
    int costKB = (int)(tileDataSize * sizeof(float) / 1024);
    fftCache.insert(TileCacheKey(fftSize, zoomLevel, tile), destStorage,
                    std::max(costKB, 1));

#if INSPECTRUM_PROFILE
    g_prof.tile_total_ns = tileTimer.nsecsElapsed();
    g_prof.report("FFT_TILE");
#endif

    return destStorage->data();
}

void SpectrogramPlot::getLine(float *dest, size_t sample)
{
    if (inputSource && fft) {
#if INSPECTRUM_PROFILE
        QElapsedTimer pt; pt.start();
#endif
        /* read windowSize samples centered on 'sample' into reusable buffer */
        const auto first_sample = std::max(
            static_cast<ssize_t>(sample) - windowSize / 2,
            static_cast<ssize_t>(0));
        if (!inputSource->getSamples(first_sample, windowSize, sampleBuf.get())) {
            auto neg_infinity = -1 * std::numeric_limits<float>::infinity();
            for (int i = 0; i < fftSize; i++, dest++)
                *dest = neg_infinity;
            return;
        }
#if INSPECTRUM_PROFILE
        g_prof.getSamples_ns += pt.nsecsElapsed(); pt.restart();
#endif

        auto *buffer = fftBuffer.get();

        /* apply window to input samples */
        for (int i = 0; i < windowSize; i++)
            buffer[i] = sampleBuf[i] * window[i];

        /* zero-pad remaining samples */
        if (fftSize > windowSize)
            memset(&buffer[windowSize], 0,
                   (fftSize - windowSize) * sizeof(std::complex<float>));
#if INSPECTRUM_PROFILE
        g_prof.window_ns += pt.nsecsElapsed(); pt.restart();
#endif

        /* execute FFT and read result directly from internal buffer */
        auto *result = reinterpret_cast<std::complex<float>*>(fft->execute(buffer));
#if INSPECTRUM_PROFILE
        g_prof.fft_ns += pt.nsecsElapsed(); pt.restart();
#endif

        /* Convert to power spectrum (dB) with FFT-shift (DC to centre).
         * Split into two sequential passes for contiguous access. */
        const int half = fftSize >> 1;

        /* first half of output <- upper half of FFT (negative frequencies) */
        for (int i = 0; i < half; i++) {
            auto s = result[half + i] * invN;
            float power = s.real() * s.real() + s.imag() * s.imag();
            dest[i] = fast_log2f_approx(power) * logMultiplier;
        }
        /* second half of output <- lower half of FFT (positive frequencies) */
        for (int i = 0; i < half; i++) {
            auto s = result[i] * invN;
            float power = s.real() * s.real() + s.imag() * s.imag();
            dest[half + i] = fast_log2f_approx(power) * logMultiplier;
        }
#if INSPECTRUM_PROFILE
        g_prof.magnitude_ns += pt.nsecsElapsed();
        g_prof.lines++;
#endif
    }
}

int SpectrogramPlot::getStride()
{
    if (zoomLevel <= 0)
        return windowSize;
    return std::max(windowSize / zoomLevel, 1);
}

float SpectrogramPlot::getTunerPhaseInc()
{
    if (fftSize <= 0)
        return 0;
    auto freq = 0.5f - tuner.centre() / (float)fftSize;
    return freq * Tau;
}

std::vector<float> SpectrogramPlot::getTunerTaps()
{
    float cutoff = (fftSize > 0) ? tuner.deviation() / (float)fftSize : 0.1f;
    float gain = pow(10.0f, powerMax / -10.0f);
    auto atten = 60.0f;
    auto len = estimate_req_filter_len(std::min(cutoff, 0.05f), atten);
    auto taps = std::vector<float>(len);
    liquid_firdes_kaiser(len, cutoff, atten, 0.0f, taps.data());
    std::transform(taps.begin(), taps.end(), taps.begin(),
                   std::bind(std::multiplies<float>(), std::placeholders::_1, gain));
    return taps;
}

int SpectrogramPlot::getLinesPerTile()
{
    return linesPerTile();
}

int SpectrogramPlot::linesPerTile()
{
    /* adaptive tile size: target ~512KB of float data per tile.
     * fftSize includes zero-pad, so memory is fftSize * lpt * 4 bytes. */
    if (fftSize <= 0)
        return targetLinesPerTile;
    int lpt = 524288 / (fftSize * (int)sizeof(float));  /* ~512KB */
    return std::max(lpt, targetLinesPerTile);
}

int SpectrogramPlot::getNativePlotHeight()
{
    int visibleBins;

    if (maskOutOfBand && tunerEnabled()) {
        visibleBins = tuner.deviation() * 2;
        if (visibleBins < 2)
            visibleBins = 2;
        if (visibleBins > fftSize)
            visibleBins = fftSize;
    } else {
        visibleBins = fftSize / yZoomLevel;
    }

    return visibleBins;
}

bool SpectrogramPlot::mouseEvent(QEvent::Type type, QMouseEvent *event)
{
    if (tunerEnabled()) {
        if (maskOutOfBand) {
            /* lock the Y offset at drag start to prevent
             * feedback loop (offset changes as tuner moves) */
            if (type == QEvent::MouseButtonPress) {
                cropDragOffset = tuner.centre() - height() / 2;
                cropDragging = true;
            } else if (type == QEvent::MouseButtonRelease) {
                cropDragging = false;
            }

            int yOffset = cropDragging
                ? cropDragOffset
                : tuner.centre() - height() / 2;

            auto translated = QMouseEvent(
                type,
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QPointF(event->position().x(), event->position().y() + yOffset),
                event->globalPosition(),
#else
                QPoint(event->pos().x(), event->pos().y() + yOffset),
#endif
                event->button(),
                event->buttons(),
                event->modifiers()
            );
            return tuner.mouseEvent(type, &translated);
        }
        return tuner.mouseEvent(type, event);
    }

    return false;
}

void SpectrogramPlot::leaveEvent()
{
    if (tunerEnabled())
        tuner.leaveEvent();
}

std::shared_ptr<AbstractSampleSource> SpectrogramPlot::output()
{
    return tunerTransform;
}

void SpectrogramPlot::setFFTSize(int size)
{
    if (size <= 0)
        return;

    /* skip if nothing changed */
    if (size == windowSize && windowSize * zeroPad == fftSize)
        return;

    int oldFFTSize = fftSize;
    windowSize = size;
    fftSize = windowSize * zeroPad;

    float sizeScale = (oldFFTSize > 0) ? float(fftSize) / float(oldFFTSize) : 1.0f;

    fft.reset(new FFT(fftSize));
    fftBuffer.reset(new std::complex<float>[fftSize]);
    sampleBuf.reset(new std::complex<float>[windowSize]);
    invN = (windowSize > 0) ? 1.0f / windowSize : 1.0f;

    size_t tileBufSize = (size_t)linesPerTile() * fftSize;
    linearBuf.resize(tileBufSize);
    tileWorkBuf.resize(tileBufSize);

    /* window is windowSize long (applied to input samples) */
    window.reset(new float[windowSize]);
    for (int i = 0; i < windowSize; i++) {
        window[i] = 0.5f * (1.0f - cos(Tau * i / std::max(windowSize - 1, 1)));
    }

    int fullHeight = inputSource->realSignal() ? fftSize / 2 : fftSize;
    tuner.setHeight(fullHeight);
    auto dev = tuner.deviation();
    auto centre = tuner.centre();
    tuner.setDeviation((int)(dev * sizeScale));
    tuner.setCentre((int)(centre * sizeScale));
    updateHeight();
}

void SpectrogramPlot::updateHeight()
{
    int fullHeight = inputSource->realSignal() ? fftSize / 2 : fftSize;

    if (maskOutOfBand && tunerEnabled()) {
        /* crop to tuner bandwidth + 20% margin on each side */
        int h = tuner.deviation() * 2.4;
        if (h < 4) h = 4;
        if (h > fullHeight) h = fullHeight;
        setHeight(h);
    } else {
        setHeight(fullHeight);
    }
}

void SpectrogramPlot::updatePowerRange()
{
    int delta = std::abs(int(powerMin - powerMax));
    powerRange = (delta > 0) ? -1.0f / delta : -1.0f;
}

void SpectrogramPlot::setPowerMax(int power)
{
    powerMax = power;
    updatePowerRange();
    pixmapCache.clear();
    tunerFullUpdate();
}

void SpectrogramPlot::setPowerMin(int power)
{
    powerMin = power;
    updatePowerRange();
    pixmapCache.clear();
}

void SpectrogramPlot::setZeroPad(int factor)
{
    if (factor < 1)
        factor = 1;
    if (factor == zeroPad)
        return;
    zeroPad = factor;
    int savedWin = windowSize;
    windowSize = 0;  /* force setFFTSize to run */
    setFFTSize(savedWin);
    pixmapCache.clear();
    enhancedCache.clear();
    fftCache.clear();
    emit repaint();
}

void SpectrogramPlot::setZoomLevel(int zoom)
{
    zoomLevel = std::max(zoom, 1);

    /* adaptive deferral: use measured render time if available,
     * else estimate from fftSize. Skip deferral for fast renders. */
    int threshold = (lastRenderMs > 0) ? lastRenderMs : fftSize / 40;

    if (threshold > 50) {
        int delay = std::min(threshold, 400);
        zoomDeferred = true;
        zoomRenderTimer.start(delay);
    }
}

void SpectrogramPlot::zoomRenderNow()
{
    zoomDeferred = false;
    emit repaint();
}

float* SpectrogramPlot::getEnhancedTile(size_t tile)
{
    if (avgCount <= 1)
        return getFFTTile(tile);

    std::vector<float> *obj = enhancedCache.object(
        TileCacheKey(fftSize, zoomLevel, tile));
    if (obj != nullptr)
        return obj->data();

#if INSPECTRUM_PROFILE
    QElapsedTimer enhTimer; enhTimer.start();
#endif
    float *raw = getFFTTile(tile);
    int lpt = linesPerTile();
    size_t tileSize = (size_t)lpt * fftSize;
    auto *enhanced = new std::vector<float>(tileSize);

    if (avgCount > 1) {
        /* dB -> linear -> average -> dB (causal box filter, width=avgCount) */
        if (linearBuf.size() < tileSize)
            linearBuf.resize(tileSize);
        for (size_t i = 0; i < tileSize; i++)
            linearBuf[i] = fast_exp2f_approx(raw[i] * dBtoLinScale);

        /* causal box filter: average avgCount lines ending at x.
         * Window = [x - avgCount + 1 .. x], clamped to [0, lpt).
         *
         * Loop order: outer x, inner y -- keeps sequential access
         * to linearBuf and enhanced (both laid out as [x * fftSize + y]).
         * Running sums array is fftSize doubles, fits comfortably in L1. */
        std::vector<double> runSum(fftSize, 0.0);
        for (int x = 0; x < lpt; x++) {
            int leaveX = x - avgCount;
            int count = std::min(x + 1, avgCount);
            double invCount = 1.0 / count;
            float *linCol = linearBuf.data() + (size_t)x * fftSize;
            float *enhCol = enhanced->data() + (size_t)x * fftSize;
            float *leaveCol = (leaveX >= 0)
                ? linearBuf.data() + (size_t)leaveX * fftSize
                : nullptr;

            if (leaveCol) {
                for (int y = 0; y < fftSize; y++) {
                    runSum[y] += linCol[y] - leaveCol[y];
                    double avg = runSum[y] * invCount;
                    if (avg < 1e-30) avg = 1e-30;
                    enhCol[y] = fast_log2f_approx((float)avg) * linToDBScale;
                }
            } else {
                for (int y = 0; y < fftSize; y++) {
                    runSum[y] += linCol[y];
                    double avg = runSum[y] * invCount;
                    if (avg < 1e-30) avg = 1e-30;
                    enhCol[y] = fast_log2f_approx((float)avg) * linToDBScale;
                }
            }
        }
    } else {
        memcpy(enhanced->data(), raw, tileSize * sizeof(float));
    }

    int enhCostKB = (int)(tileSize * sizeof(float) / 1024);
    enhancedCache.insert(TileCacheKey(fftSize, zoomLevel, tile), enhanced,
                         std::max(enhCostKB, 1));
#if INSPECTRUM_PROFILE
    g_prof.enhanced_ns = enhTimer.nsecsElapsed();
    qDebug("[PROFILE ENHANCED] fft=%d lpt=%d | enhanced=%lld us",
           fftSize, lpt, g_prof.enhanced_ns/1000);
#endif
    return enhanced->data();
}


void SpectrogramPlot::setAveraging(int count)
{
    if (count < 1) count = 1;
    if (count == avgCount) return;
    avgCount = count;
    enhancedCache.clear();
    pixmapCache.clear();
    emit repaint();
}


void SpectrogramPlot::setZoomY(int level)
{
    yZoomLevel = std::max(level, 1);

    /* no cache clear needed -- tiles are the same, only
     * the crop window in paintMid changes */
    emit repaint();
}

void SpectrogramPlot::setSampleRate(double rate)
{
    sampleRate = rate;
}

void SpectrogramPlot::enableScales(bool enabled)
{
   frequencyScaleEnabled = enabled;
}

void SpectrogramPlot::enableMaskOutOfBand(bool enabled)
{
    maskOutOfBand = enabled;
    updateHeight();
    emit repaint();
}

void SpectrogramPlot::enableAnnotations(bool enabled)
{
   sigmfAnnotationsEnabled = enabled;
}

bool SpectrogramPlot::isAnnotationsEnabled(void)
{
    return sigmfAnnotationsEnabled;
}

bool SpectrogramPlot::tunerEnabled()
{
    return (tunerTransform->subscriberCount() > 0);
}

void SpectrogramPlot::setTunerVisible(bool visible)
{
    tunerVisible = visible;
    emit repaint();
}

void SpectrogramPlot::tunerMoved()
{
    /*
     * Lightweight update: just redraw the tuner overlay and
     * update the info display. Do NOT call updateHeight() here
     * -- changing the height during drag causes a feedback loop
     * (height change -> coordinate mapping change -> cursor jumps).
     * Height is updated in tunerFullUpdate() when drag ends.
     */
    emit tunerInfoChanged(tunerCentreHz(), tunerBandwidthHz());
    emit repaint();

    /* restart the deferred update timer */
    tunerUpdateTimer.start(150);
}

void SpectrogramPlot::tunerFullUpdate()
{
    /* skip all expensive work while still dragging -- only
     * update when the user releases the mouse button */
    if (tuner.isDragging()) {
        tunerUpdateTimer.start(100); /* retry later */
        return;
    }

    int fullHeight = inputSource->realSignal() ? fftSize / 2 : fftSize;

    tunerTransform->setFrequency(getTunerPhaseInc());
    tunerTransform->setTaps(getTunerTaps());
    tunerTransform->setRelativeBandwith(
        tuner.deviation() * 2.0 / std::max(fullHeight, 1));

    updateHeight();
    QPixmapCache::clear();
    emit repaint();
}

uint qHash(const TileCacheKey &key, uint seed)
{
    /* FNV-1a style mixing for better distribution */
    uint h = seed ^ 2166136261u;
    h = (h ^ (uint)key.fftSize) * 16777619u;
    h = (h ^ (uint)key.zoomLevel) * 16777619u;
    h = (h ^ (uint)(key.sample)) * 16777619u;
    h = (h ^ (uint)(key.sample >> 32)) * 16777619u;
    return h;
}
