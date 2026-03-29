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

#include <QCache>
#include <QString>
#include <QTimer>
#include <QWidget>
#include "fft.h"
#include "inputsource.h"
#include "plot.h"
#include "tuner.h"
#include "tunertransform.h"

#include <memory>
#include <array>
#include <math.h>
#include <vector>

class AnnotationLocation;

class TileCacheKey
{

public:
    TileCacheKey(int fftSize, int zoomLevel, size_t sample) {
        this->fftSize = fftSize;
        this->zoomLevel = zoomLevel;
        this->sample = sample;
    }

    bool operator==(const TileCacheKey &k2) const {
        return (this->fftSize == k2.fftSize) &&
               (this->zoomLevel == k2.zoomLevel) &&
               (this->sample == k2.sample);
    }

    int fftSize;
    int zoomLevel;
    size_t sample;
};

class SpectrogramPlot : public Plot
{
    Q_OBJECT

public:
    SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>> src);
    void invalidateEvent() override;
    std::shared_ptr<AbstractSampleSource> output() override;
    void paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    bool mouseEvent(QEvent::Type type, QMouseEvent *event) override;
    void leaveEvent();
    std::shared_ptr<SampleSource<std::complex<float>>> input() { return inputSource; };
    void setSampleRate(double sampleRate);
    bool tunerEnabled();
    void setTunerVisible(bool visible);
    bool isTunerVisible() { return tunerVisible; }
    int tunerCentre() { return tuner.centre(); }
    int tunerDeviation() { return tuner.deviation(); }
    void setTunerCentre(int centre) { tuner.setCentre(centre); }
    void setTunerDeviation(int dev) { tuner.setDeviation(dev); }
    int getFFTSize() { return fftSize; }
    int getNativePlotHeight();
    using Plot::setHeight;
    int getLinesPerTile();
    double tunerCentreHz() {
        return (fftSize > 0) ? (0.5 - tuner.centre() / (double)fftSize) * sampleRate : 0;
    }
    double tunerBandwidthHz() {
        return (fftSize > 0) ? tuner.deviation() * 2.0 / fftSize * sampleRate : 0;
    }
    void enableScales(bool enabled);
    void enableMaskOutOfBand(bool enabled);
    void enableAnnotations(bool enabled);
    bool isAnnotationsEnabled();
    QString *mouseAnnotationComment(const QMouseEvent *event);

signals:
    void tunerInfoChanged(double centreHz, double bandwidthHz);
    void renderTimeChanged(int ms);

public slots:
    void tunerFullUpdate();
    void zoomRenderNow();
    void setFFTSize(int size);
    void setZeroPad(int factor);
    void setZoomY(int level);
    void setPowerMax(int power);
    void setPowerMin(int power);
    void setZoomLevel(int zoom);
    void tunerMoved();
    void setAveraging(int count);

private:
    const int linesPerGraduation = 50;
    static const int targetLinesPerTile = 64; // target FFT lines per tile

    std::shared_ptr<SampleSource<std::complex<float>>> inputSource;
    std::vector<AnnotationLocation> visibleAnnotationLocations;
    std::unique_ptr<FFT> fft;
    std::unique_ptr<float[]> window;
    std::unique_ptr<std::complex<float>[]> fftBuffer;
    std::unique_ptr<std::complex<float>[]> sampleBuf;
    QCache<TileCacheKey, QPixmap> pixmapCache;
    QCache<TileCacheKey, std::vector<float>> fftCache;
    uint colormap[256];

    int fftSize;        /* actual FFT length (windowSize * zeroPad) */
    int windowSize;     /* number of IQ samples per FFT window */
    int zeroPad = 1;    /* zero-pad factor: 1, 2, 4, 8 */
    int zoomLevel;
    int yZoomLevel = 1;
    float powerMax;
    float powerMin;

    /* precomputed constants (updated when settings change) */
    float invN = 1.0f;                         /* 1.0 / windowSize */
    static constexpr float logMultiplier = 3.0102999566398120f; /* 10 / log2(10) = dBFS */
    static constexpr float dBtoLinScale = 0.33219280948873626f; /* log2(10) / 10 = 1/logMultiplier */
    static constexpr float linToDBScale = 3.0102999566398120f;  /* 10 / log2(10) = dBFS */
    float powerRange = -1.0f;                  /* -1.0 / abs(powerMin - powerMax) */

    /* reusable buffers (avoid per-tile allocation) */
    std::vector<float> linearBuf;              /* dB->linear conversion */
    std::vector<float> tileWorkBuf;            /* scratch for getFFTTile */
    std::vector<QRgb*> scanLinePtrs;           /* precomputed QImage row pointers */
    double sampleRate;
    bool frequencyScaleEnabled;
    bool sigmfAnnotationsEnabled;
    bool maskOutOfBand = false;
    bool tunerVisible = true;
    bool cropDragging = false;
    int cropDragOffset = 0;

    Tuner tuner;
    std::shared_ptr<TunerTransform> tunerTransform;
    QTimer tunerUpdateTimer;
    QTimer zoomRenderTimer;
    bool zoomDeferred = false;
    int lastRenderMs = 0;  /* measured render time for adaptive delay */

    /* enhancement mode */
    int avgCount = 1;           /* averaging factor (1 = off) */
    QCache<TileCacheKey, std::vector<float>> enhancedCache;

    void updateHeight();
    void updatePowerRange();
    QPixmap* getPixmapTile(size_t tile);
    float* getFFTTile(size_t tile);
    float* getEnhancedTile(size_t tile);
    void getLine(float *dest, size_t sample);
    int getStride();
    float getTunerPhaseInc();
    std::vector<float> getTunerTaps();
    int linesPerTile();
    void paintFrequencyScale(QPainter &painter, QRect &rect);
    void paintAnnotations(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
};

class AnnotationLocation
{
public:
    Annotation annotation;

    AnnotationLocation(Annotation annotation, int x, int y, int width, int height)
        : annotation(annotation), x(x), y(y), width(width), height(height) {}

    bool isInside(int pos_x, int pos_y) {
        return (x <= pos_x) && (pos_x <= x + width)
            && (y <= pos_y) && (pos_y <= y + height);
    }

private:
    int x;
    int y;
    int width;
    int height;
};
