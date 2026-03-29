/*
 *  Copyright (C) 2015-2016, Mike Walters <mike@flomp.net>
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

#include "plotview.h"
#include <climits>
#include "amplitudedemod.h"
#include "frequencydemod.h"
#include "phasedemod.h"
#include <liquid/liquid.h>
#include <iostream>
#include <fstream>
#include <QtGlobal>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QGroupBox>
#include <QMenu>
#include <QPainter>
#include <QProgressDialog>
#include <QRadioButton>
#include <QPixmapCache>
#include <QScrollBar>
#include <QSpinBox>
#include <QThreadPool>
#include <QTextStream>
#include <QToolTip>
#include <QVBoxLayout>
#include "plots.h"

PlotView::PlotView(InputSource *input) : cursors(this), viewRange({0, 0})
{
    mainSampleSource = input;
    setDragMode(QGraphicsView::ScrollHandDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setMouseTracking(true);
    enableCursors(false);
    connect(&cursors, &Cursors::cursorsMoved, this, &PlotView::cursorsMoved);

    spectrogramPlot = new SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>>(mainSampleSource));
    auto tunerOutput = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(spectrogramPlot->output());

    /* forward tuner info and render time to controls panel */
    connect(spectrogramPlot, &SpectrogramPlot::tunerInfoChanged,
            this, &PlotView::tunerChanged);
    connect(spectrogramPlot, &SpectrogramPlot::renderTimeChanged,
            this, &PlotView::renderTimeChanged);

    enableScales(true);

    enableAnnotations(true);
    enableAnnotationCommentsTooltips(true);

    addPlot(spectrogramPlot);

    mainSampleSource->subscribe(this);
}

void PlotView::addPlot(Plot *plot)
{
    plots.emplace_back(plot);
    connect(plot, &Plot::repaint, this, &PlotView::repaint);
    updateThresholdPlots();
}

void PlotView::mouseMoveEvent(QMouseEvent *event)
{
    updateAnnotationTooltip(event);
    QGraphicsView::mouseMoveEvent(event);
}

void PlotView::mouseReleaseEvent(QMouseEvent *event)
{
    // This is used to show the tooltip again on drag release if the mouse is
    // hovering over an annotation.
    updateAnnotationTooltip(event);
    QGraphicsView::mouseReleaseEvent(event);
}

void PlotView::updateAnnotationTooltip(QMouseEvent *event)
{
    // If there are any mouse buttons pressed, we assume
    // that the plot is being dragged and hide the tooltip.
    bool isDrag = event->buttons() != Qt::NoButton;
    if (!annotationCommentsEnabled
        || !spectrogramPlot
        || !spectrogramPlot->isAnnotationsEnabled()
        || isDrag)  {
        QToolTip::hideText();
    } else if (spectrogramPlot) {
        QString* comment = spectrogramPlot->mouseAnnotationComment(event);
        if (comment != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QToolTip::showText(event->globalPosition().toPoint(), *comment);
#else
            QToolTip::showText(event->globalPos(), *comment);
#endif
        } else {
            QToolTip::hideText();
        }
    }
}

void PlotView::contextMenuEvent(QContextMenuEvent * event)
{
    QMenu menu;

    // Get selected plot
    Plot *selectedPlot = nullptr;
    auto it = plots.begin();
    int y = -verticalScrollBar()->value();
    for (; it != plots.end(); it++) {
        auto&& plot = *it;
        if (range_t<int>{y, y + plot->height()}.contains(event->pos().y())) {
            selectedPlot = plot.get();
            break;
        }
        y += plot->height();
    }
    if (selectedPlot == nullptr)
        return;

    // Add actions to add derived plots
    // that are compatible with selectedPlot's output
    QMenu *plotsMenu = menu.addMenu("Add derived plot");
    auto src = selectedPlot->output();
    int parentIdx = std::distance(plots.begin(), it);
    if (!src)
        return;
    auto compatiblePlots = as_range(Plots::plots.equal_range(src->sampleType()));
    for (auto p : compatiblePlots) {
        auto plotInfo = p.second;
        auto action = new QAction(QString("Add %1").arg(plotInfo.name), plotsMenu);
        auto plotCreator = plotInfo.creator;
        QString typeName = plotInfo.name;
        connect(
            action, &QAction::triggered,
            this, [=]() {
                addPlot(plotCreator(src));
                derivedPlotInfos.push_back({parentIdx, typeName});
            }
        );
        plotsMenu->addAction(action);
    }

    // Add submenu for extracting symbols
    QMenu *extractMenu = menu.addMenu("Extract symbols");
    // Add action to extract symbols from selected plot to stdout
    auto extract = new QAction("To stdout", extractMenu);
    connect(
        extract, &QAction::triggered,
        this, [=]() {
            extractSymbols(src, false);
        }
    );
    extract->setEnabled(cursorsEnabled && (src->sampleType() == typeid(float)));
    extractMenu->addAction(extract);

    // Add action to extract symbols from selected plot to clipboard
    auto extractClipboard = new QAction("Copy to clipboard", extractMenu);
    connect(
        extractClipboard, &QAction::triggered,
        this, [=]() {
            extractSymbols(src, true);
        }
    );
    extractClipboard->setEnabled(cursorsEnabled && (src->sampleType() == typeid(float)));
    extractMenu->addAction(extractClipboard);

    // Add submenu for exporting binary/hex from threshold plots
    auto threshPlot = dynamic_cast<ThresholdPlot*>(selectedPlot);
    if (threshPlot) {
        QMenu *binHexMenu = menu.addMenu("Export bin/hex/ascii");

        auto copyBin = new QAction("Copy binary to clipboard", binHexMenu);
        connect(copyBin, &QAction::triggered, this, [=]() {
            QGuiApplication::clipboard()->setText(threshPlot->getBinaryString());
        });
        copyBin->setEnabled(cursorsEnabled);
        binHexMenu->addAction(copyBin);

        auto copyHex = new QAction("Copy hex to clipboard", binHexMenu);
        connect(copyHex, &QAction::triggered, this, [=]() {
            QGuiApplication::clipboard()->setText(threshPlot->getHexString());
        });
        copyHex->setEnabled(cursorsEnabled);
        binHexMenu->addAction(copyHex);

        auto copyAscii = new QAction("Copy ASCII to clipboard", binHexMenu);
        connect(copyAscii, &QAction::triggered, this, [=]() {
            QGuiApplication::clipboard()->setText(threshPlot->getAsciiString());
        });
        copyAscii->setEnabled(cursorsEnabled);
        binHexMenu->addAction(copyAscii);

        auto binToStdout = new QAction("Binary to stdout", binHexMenu);
        connect(binToStdout, &QAction::triggered, this, [=]() {
            std::cout << threshPlot->getBinaryString().toStdString() << std::endl;
        });
        binToStdout->setEnabled(cursorsEnabled);
        binHexMenu->addAction(binToStdout);

        auto hexToStdout = new QAction("Hex to stdout", binHexMenu);
        connect(hexToStdout, &QAction::triggered, this, [=]() {
            std::cout << threshPlot->getHexString().toStdString() << std::endl;
        });
        hexToStdout->setEnabled(cursorsEnabled);
        binHexMenu->addAction(hexToStdout);
    }

    // Add action to export the selected samples into a file
    auto save = new QAction("Export samples to file...", &menu);
    connect(
        save, &QAction::triggered,
        this, [=]() {
            if (selectedPlot == spectrogramPlot) {
                exportSamples(spectrogramPlot->tunerEnabled() ? spectrogramPlot->output() : spectrogramPlot->input());
            } else {
                exportSamples(src);
            }
        }
    );
    menu.addAction(save);

    // Add action to export tuner-filtered + resampled
    if (spectrogramPlot->tunerEnabled()) {
        auto saveTuner = new QAction("Export tuner-filtered to file...", &menu);
        connect(
            saveTuner, &QAction::triggered,
            this, [=]() {
                exportTunerFiltered();
            }
        );
        menu.addAction(saveTuner);
    }

    // Add action to export full spectrogram as PNG
    if (selectedPlot == spectrogramPlot) {
        auto exportPng = new QAction("Export spectrogram to PNG...", &menu);
        connect(exportPng, &QAction::triggered, this, [this]() {
            exportSpectrogramPng();
        });
        menu.addAction(exportPng);
    }

    // Add action to remove the selected plot
    int removeIdx = std::distance(plots.begin(), it);
    auto rem = new QAction("Remove plot", &menu);
    connect(
        rem, &QAction::triggered,
        this, [=]() {
            if (removeIdx < 0 || removeIdx >= (int)plots.size())
                return;
            QPixmapCache::clear();
            QThreadPool::globalInstance()->waitForDone();
            if (removeIdx > 0 && (removeIdx - 1) < (int)derivedPlotInfos.size())
                derivedPlotInfos.erase(derivedPlotInfos.begin() + removeIdx - 1);
            plots.erase(plots.begin() + removeIdx);
        }
    );
    // Don't allow remove the first plot (the spectrogram)
    rem->setEnabled(it != plots.begin());
    menu.addAction(rem);

    updateViewRange(false);
    if(menu.exec(event->globalPos()))
        updateView(false);
}

void PlotView::updateThresholdPlots()
{
    for (auto&& plot : plots) {
        auto tp = dynamic_cast<ThresholdPlot*>(plot.get());
        if (tp)
            tp->setCursorInfo(cursorsEnabled, selectedSamples,
                              cursors.segments());
    }
}

void PlotView::refreshThresholdPlots()
{
    /* invalidate bit cache -- data chain wasn't ready at plot creation */
    for (auto&& plot : plots) {
        auto tp = dynamic_cast<ThresholdPlot*>(plot.get());
        if (tp)
            tp->setCursorInfo(false, {0, 0}, 0);
    }
    updateThresholdPlots();
    emitTimeSelection();
    viewport()->update();
}

void PlotView::cursorsMoved()
{
    int scrollVal = horizontalScrollBar()->value();
    int curMin = cursors.selection().minimum;
    int curMax = cursors.selection().maximum;

    int colMin = (curMin >= 0 && scrollVal <= INT_MAX - curMin)
        ? scrollVal + curMin : scrollVal;
    int colMax = (curMax >= 0 && scrollVal <= INT_MAX - curMax)
        ? scrollVal + curMax : scrollVal;

    selectedSamples = {
        columnToSample(colMin),
        columnToSample(colMax)
    };

    if (mainSampleSource) {
        size_t maxCount = mainSampleSource->count();
        if (selectedSamples.minimum > maxCount)
            selectedSamples.minimum = maxCount;
        if (selectedSamples.maximum > maxCount)
            selectedSamples.maximum = maxCount;
    }

    if (selectedSamples.minimum > selectedSamples.maximum)
        selectedSamples.minimum = selectedSamples.maximum;

    updateThresholdPlots();
    emitTimeSelection();
    viewport()->update();
}

void PlotView::emitTimeSelection()
{
    if (mainSampleSource == nullptr || mainSampleSource->rate() == 0)
        return;
    size_t sampleCount = selectedSamples.length();
    float selectionTime = sampleCount / (float)mainSampleSource->rate();
    float offsetTime = selectedSamples.minimum / (float)mainSampleSource->rate();
    emit timeSelectionChanged(selectionTime, offsetTime);
}

void PlotView::resetCursorState()
{
    hadCursors = false;
    savedSelectedSamples = {0, 0};
}

void PlotView::enableCursors(bool enabled)
{
    if (!enabled && cursorsEnabled) {
        savedSelectedSamples = selectedSamples;
        hadCursors = true;
    }

    cursorsEnabled = enabled;

    if (enabled) {
        if (hadCursors && savedSelectedSamples.length() > 0) {
            selectedSamples = savedSelectedSamples;
            updateView();
            emitTimeSelection();
        } else {
            /* first time: default to 1/3 margins */
            int margin = viewport()->rect().width() / 3;
            cursors.setSelection({viewport()->rect().left() + margin,
                                  viewport()->rect().right() - margin});
            cursorsMoved();
        }
    }
    updateThresholdPlots();
    viewport()->update();
}

void PlotView::setCursorGridOpacity(int opacity)
{
    cursors.setGridOpacity(opacity);
    viewport()->update();
}

bool PlotView::viewportEvent(QEvent *event) {
    // Handle wheel events for zooming (before the parent's handler to stop normal scrolling)
    if (event->type() == QEvent::Wheel) {
        QWheelEvent *wheelEvent = (QWheelEvent*)event;
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
            bool canZoomIn = zoomLevel < fftSize;
            bool canZoomOut = zoomLevel > 1;
            int delta = wheelEvent->angleDelta().y();
            if ((delta > 0 && canZoomIn) || (delta < 0 && canZoomOut)) {
                scrollZoomStepsAccumulated += delta;

                // `updateViewRange()` keeps the center sample in the same place after zoom. Apply
                // a scroll adjustment to keep the sample under the mouse cursor in the same place instead.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                zoomPos = wheelEvent->position().x();
#else
                zoomPos = wheelEvent->pos().x();
#endif
                zoomSample = columnToSample(horizontalScrollBar()->value() + zoomPos);
                if (scrollZoomStepsAccumulated >= 120) {
                    scrollZoomStepsAccumulated -= 120;
                    emit zoomIn();
                } else if (scrollZoomStepsAccumulated <= -120) {
                    scrollZoomStepsAccumulated += 120;
                    emit zoomOut();
                }
            }
            return true;
        }
    }

    // Pass mouse events to individual plot objects
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease) {

        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);

        int plotY = -verticalScrollBar()->value();
        for (auto&& plot : plots) {
            auto mouse_event = QMouseEvent(
                event->type(),
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QPointF(mouseEvent->position().x(), mouseEvent->position().y() - plotY),
                mouseEvent->globalPosition(),
#else
                QPoint(mouseEvent->pos().x(), mouseEvent->pos().y() - plotY),
#endif
                mouseEvent->button(),
                mouseEvent->buttons(),
                QApplication::keyboardModifiers()
            );
            bool result = plot->mouseEvent(
                event->type(),
                &mouse_event
            );
            if (result)
                return true;
            plotY += plot->height();
        }

        if (cursorsEnabled)
            if (cursors.mouseEvent(event->type(), mouseEvent))
                return true;
    }

    if (event->type() == QEvent::Leave) {
        for (auto&& plot : plots) {
            plot->leaveEvent();
        }

        if (cursorsEnabled)
            cursors.leaveEvent();
    }

    // Handle parent eveents
    return QGraphicsView::viewportEvent(event);
}

void PlotView::extractSymbols(std::shared_ptr<AbstractSampleSource> src,
                              bool toClipboard)
{
    if (!cursorsEnabled)
        return;
    auto floatSrc = std::dynamic_pointer_cast<SampleSource<float>>(src);
    if (!floatSrc)
        return;
    if (selectedSamples.length() == 0 || cursors.segments() == 0)
        return;
    auto samples = floatSrc->getSamples(selectedSamples.minimum, selectedSamples.length());
    if (!samples)
        return;
    auto step = (float)selectedSamples.length() / cursors.segments();
    if (step == 0)
        return;
    auto symbols = std::vector<float>();
    for (auto i = step / 2; i < selectedSamples.length(); i += step)
    {
        size_t idx = (size_t)i;
        if (idx >= selectedSamples.length())
            break;
        symbols.push_back(samples[idx]);
    }
    if (!toClipboard) {
        for (auto f : symbols)
            std::cout << f << ", ";
        std::cout << std::endl << std::flush;
    } else {
        QClipboard *clipboard = QGuiApplication::clipboard();
        QString symbolText;
        QTextStream symbolStream(&symbolText);
        for (auto f : symbols)
            symbolStream << f << ", ";
        clipboard->setText(symbolText);
    }
}

void PlotView::exportSamples(std::shared_ptr<AbstractSampleSource> src)
{
    if (src->sampleType() == typeid(std::complex<float>)) {
        exportSamples<std::complex<float>>(src);
    } else {
        exportSamples<float>(src);
    }
}

template<typename SOURCETYPE>
void PlotView::exportSamples(std::shared_ptr<AbstractSampleSource> src)
{
    auto sampleSrc = std::dynamic_pointer_cast<SampleSource<SOURCETYPE>>(src);
    if (!sampleSrc) {
        return;
    }

    QFileDialog dialog(this);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(getFileNameFilter<SOURCETYPE>());
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);

    QGroupBox groupBox("Selection To Export", &dialog);
    QVBoxLayout vbox(&groupBox);

    QRadioButton cursorSelection("Cursor Selection", &groupBox);
    QRadioButton currentView("Current View", &groupBox);
    QRadioButton completeFile("Complete File", &groupBox);

    if (cursorsEnabled) {
        cursorSelection.setChecked(true);
    } else {
        currentView.setChecked(true);
        cursorSelection.setEnabled(false);
    }

    vbox.addWidget(&cursorSelection);
    vbox.addWidget(&currentView);
    vbox.addWidget(&completeFile);
    vbox.addStretch(1);

    groupBox.setLayout(&vbox);

    QGridLayout *l = dialog.findChild<QGridLayout*>();
    l->addWidget(&groupBox, 4, 1);

    QGroupBox groupBox2("Decimation");
    QSpinBox decimation(&groupBox2);
    decimation.setMinimum(1);
    decimation.setValue(1 / sampleSrc->relativeBandwidth());

    QVBoxLayout vbox2;
    vbox2.addWidget(&decimation);

    groupBox2.setLayout(&vbox2);
    l->addWidget(&groupBox2, 4, 2);

    if (dialog.exec()) {
        QStringList fileNames = dialog.selectedFiles();

        size_t start, end;
        if (cursorSelection.isChecked()) {
            start = selectedSamples.minimum;
            end = start + selectedSamples.length();
        } else if(currentView.isChecked()) {
            start = viewRange.minimum;
            end = start + viewRange.length();
        } else {
            start = 0;
            end = sampleSrc->count();
        }

        std::ofstream os (fileNames[0].toStdString(), std::ios::binary);

        /*
         * Export in chunks. Use a large chunk size to minimize
         * demodulator warm-up artifacts at chunk boundaries.
         */
        size_t step = std::max(viewRange.length(), (size_t)65536);
        int decVal = std::max(decimation.value(), 1);

        QProgressDialog progress("Exporting samples...", "Cancel",
                                 0, (int)((end - start) / step + 1), this);
        progress.setWindowModality(Qt::WindowModal);
        int progressIdx = 0;

        for (size_t index = start; index < end; index += step) {
            progress.setValue(progressIdx++);
            if (progress.wasCanceled())
                break;

            size_t length = std::min(step, end - index);
            auto samples = sampleSrc->getSamples(index, length);
            if (samples != nullptr) {
                for (size_t i = 0; i < length; i += decVal) {
                    os.write((const char*)&samples[i], sizeof(SOURCETYPE));
                }
            }
        }
    }
}

void PlotView::exportTunerFiltered()
{
    auto tunerSrc = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(
        spectrogramPlot->output());
    if (!tunerSrc)
        return;

    float relBw = tunerSrc->relativeBandwidth();
    if (relBw <= 0 || relBw > 1)
        relBw = 1.0f;

    double outputRate = sampleRate * relBw;
    float resampRate = relBw;  /* < 1.0 = decimation */

    QFileDialog dialog(this);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(getFileNameFilter<std::complex<float>>());
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);

    /* range selection */
    QGroupBox groupBox("Selection To Export", &dialog);
    QVBoxLayout vbox(&groupBox);

    QRadioButton cursorSelection("Cursor Selection", &groupBox);
    QRadioButton currentView("Current View", &groupBox);
    QRadioButton completeFile("Complete File", &groupBox);

    if (cursorsEnabled) {
        cursorSelection.setChecked(true);
    } else {
        currentView.setChecked(true);
        cursorSelection.setEnabled(false);
    }

    vbox.addWidget(&cursorSelection);
    vbox.addWidget(&currentView);
    vbox.addWidget(&completeFile);
    vbox.addStretch(1);
    groupBox.setLayout(&vbox);

    QGridLayout *l = dialog.findChild<QGridLayout*>();
    l->addWidget(&groupBox, 4, 1);

    /* info box */
    QGroupBox infoBox("Tuner Export Info");
    QFormLayout infoLayout;

    QLabel centreLabel(QString::fromStdString(
        formatSIValue(spectrogramPlot->tunerCentreHz())) + "Hz");
    infoLayout.addRow("Centre freq:", &centreLabel);

    QLabel bwLabel(QString::fromStdString(
        formatSIValue(spectrogramPlot->tunerBandwidthHz())) + "Hz");
    infoLayout.addRow("Bandwidth:", &bwLabel);

    QLabel outRateLabel(QString::fromStdString(
        formatSIValue(outputRate)) + "Hz");
    infoLayout.addRow("Output sample rate:", &outRateLabel);

    QLabel ratioLabel(QString("1:%1").arg((int)(1.0f / relBw + 0.5f)));
    infoLayout.addRow("Decimation:", &ratioLabel);

    infoBox.setLayout(&infoLayout);
    l->addWidget(&infoBox, 4, 2);

    if (!dialog.exec())
        return;

    QStringList fileNames = dialog.selectedFiles();
    if (fileNames.isEmpty())
        return;

    size_t start, end;
    if (cursorSelection.isChecked()) {
        start = selectedSamples.minimum;
        end = selectedSamples.minimum + selectedSamples.length();
    } else if (currentView.isChecked()) {
        start = viewRange.minimum;
        end = viewRange.minimum + viewRange.length();
    } else {
        start = 0;
        end = tunerSrc->count();
    }

    if (end <= start)
        return;

    std::ofstream os(fileNames[0].toStdString(), std::ios::binary);
    if (!os.is_open())
        return;

    /* create rational resampler (liquid-dsp)
     * destroyed at end of function -- all early exits are above */
    msresamp_crcf resampler = msresamp_crcf_create(resampRate, 60.0f);
    if (!resampler)
        return;

    size_t step = std::max(viewRange.length(), (size_t)65536);
    size_t totalSteps = (end - start + step - 1) / step;

    QProgressDialog progress("Exporting tuner-filtered...", "Cancel",
                             0, (int)totalSteps, this);
    progress.setWindowModality(Qt::WindowModal);
    int progressIdx = 0;

    /* output buffer: resampled output is at most input size */
    std::vector<std::complex<float>> outBuf(step + 256);

    for (size_t index = start; index < end; index += step) {
        progress.setValue(progressIdx++);
        if (progress.wasCanceled())
            break;

        size_t length = std::min(step, end - index);
        auto samples = tunerSrc->getSamples(index, length);
        if (!samples)
            continue;

        /* resample chunk */
        unsigned int numWritten = 0;
        msresamp_crcf_execute(resampler,
                              samples.get(), length,
                              outBuf.data(), &numWritten);

        if (numWritten > 0) {
            os.write((const char *)outBuf.data(),
                     numWritten * sizeof(std::complex<float>));
        }
    }

    msresamp_crcf_destroy(resampler);
}

void PlotView::exportSpectrogramPng()
{
    if (!mainSampleSource || !spectrogramPlot)
        return;

    /* default filename from signal file */
    QString defaultPath;
    auto *inputSrc = dynamic_cast<InputSource *>(mainSampleSource);
    if (inputSrc && !inputSrc->getFileName().isEmpty()) {
        QFileInfo fi(inputSrc->getFileName());
        defaultPath = fi.absolutePath() + "/" + fi.completeBaseName() + ".png";
    }

    QString fileName = QFileDialog::getSaveFileName(this,
        "Export Spectrogram to PNG", defaultPath, "PNG Image (*.png)");
    if (fileName.isEmpty())
        return;

    /* export exactly what's on screen: same sample range, same
     * pixel dimensions, same zoom/crop/power settings */
    size_t spc = samplesPerColumn();
    if (spc == 0) spc = 1;

    int imgWidth = (int)(viewRange.length() / spc);
    if (imgWidth <= 0)
        return;

    int plotHeight = spectrogramPlot->height();
    if (plotHeight <= 0) plotHeight = 256;
    int scaleHeight = 30;
    int imgHeight = scaleHeight + plotHeight + scaleHeight;

    QImage image(imgWidth, imgHeight, QImage::Format_RGB32);
    image.fill(Qt::black);

    QPainter painter(&image);
    QRect plotRect(0, scaleHeight, imgWidth, plotHeight);

    int lpt = spectrogramPlot->getLinesPerTile();
    if (lpt <= 0) lpt = 64;
    int totalTiles = imgWidth / lpt + 2;

    QProgressDialog progress("Exporting spectrogram...", "Cancel",
                             0, totalTiles + 5, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    QApplication::processEvents();

    /* clip painting to the spectrogram area so scales stay clean */
    painter.setClipRect(plotRect);

    /* paint back + mid + front using the current view range
     * but mapped to image coordinates starting at x=0 */
    spectrogramPlot->paintBack(painter, plotRect, viewRange);
    progress.setValue(1);

    int tilesRendered = 0;
    int lastProgress = 1;

    size_t chunkSamples = (size_t)lpt * spc * 10;
    if (chunkSamples == 0) chunkSamples = spc;

    for (size_t s = 0; s < viewRange.length() && !progress.wasCanceled();
         s += chunkSamples) {
        size_t chunkEnd = std::min(s + chunkSamples, viewRange.length());

        range_t<size_t> chunkRange = {
            viewRange.minimum + s,
            viewRange.minimum + chunkEnd
        };

        int x1 = (int)(s / spc);
        int x2 = (int)(chunkEnd / spc);
        QRect chunkRect(x1, scaleHeight, x2 - x1, plotHeight);

        spectrogramPlot->paintMid(painter, chunkRect, chunkRange);

        tilesRendered += 10;
        int p = std::min(1 + tilesRendered, totalTiles);
        if (p > lastProgress) {
            progress.setValue(p);
            lastProgress = p;
            QApplication::processEvents();
        }
    }

    if (progress.wasCanceled()) {
        painter.end();
        return;
    }

    spectrogramPlot->paintFront(painter, plotRect, viewRange);
    progress.setValue(totalTiles + 2);
    QApplication::processEvents();

    /* remove clip so scales can draw outside the spectrogram area */
    painter.setClipping(false);

    /* draw time scales on top and bottom -- same logic as paintTimeScale */
    if (sampleRate > 0) {
        float startTime = (float)viewRange.minimum / sampleRate;
        float stopTime = (float)viewRange.maximum / sampleRate;
        float duration = stopTime - startTime;

        if (duration > 0) {
            int tickWidth = 80;
            int maxTicks = imgWidth / tickWidth;
            if (maxTicks < 1) maxTicks = 1;

            double durationPerTick = 10 * pow(10, floor(log(duration / maxTicks) / log(10)));

            if (durationPerTick > 0) {
                painter.save();
                painter.setPen(QPen(Qt::white, 1, Qt::SolidLine));

                double firstTick = (int)(startTime / durationPerTick) * durationPerTick;

                int topY = 0;
                int botY = scaleHeight + plotHeight;

                /* major ticks + labels */
                for (double tick = firstTick; tick <= stopTime; tick += durationPerTick) {
                    if (tick < 0) continue;
                    size_t tickSample = (size_t)(tick * sampleRate);
                    if (tickSample < viewRange.minimum) continue;
                    int x = (int)((tickSample - viewRange.minimum) / spc);
                    if (x < 0 || x >= imgWidth) continue;

                    char buf[128];
                    snprintf(buf, sizeof(buf), "%.06f", tick);

                    painter.drawLine(x, topY, x, topY + scaleHeight);
                    painter.drawText(x + 2, topY + scaleHeight - 5, buf);

                    painter.drawLine(x, botY, x, botY + scaleHeight);
                    painter.drawText(x + 2, botY + scaleHeight - 5, buf);
                }

                /* minor ticks */
                double dtMinor = durationPerTick / 10;
                if (dtMinor > 0) {
                    double firstMinor = (int)(startTime / dtMinor) * dtMinor;
                    for (double tick = firstMinor; tick <= stopTime; tick += dtMinor) {
                        if (tick < 0) continue;
                        size_t tickSample = (size_t)(tick * sampleRate);
                        if (tickSample < viewRange.minimum) continue;
                        int x = (int)((tickSample - viewRange.minimum) / spc);
                        if (x < 0 || x >= imgWidth) continue;

                        painter.drawLine(x, topY, x, topY + 10);
                        painter.drawLine(x, botY, x, botY + 10);
                    }
                }

                painter.restore();
            }
        }
    }

    painter.end();

    if (!progress.wasCanceled())
        image.save(fileName, "PNG");
}

void PlotView::invalidateEvent()
{
    horizontalScrollBar()->setMinimum(0);
    horizontalScrollBar()->setMaximum(sampleToColumn(mainSampleSource->count()));
}

void PlotView::repaint()
{
    viewport()->update();
}

void PlotView::setSegmentsOnly(int segments)
{
    cursors.setSegments(segments);
    updateThresholdPlots();
    updateView();
    emitTimeSelection();
}

void PlotView::setCursorSegments(int segments)
{
    if (segments < 1)
        segments = 1;

    int oldSegments = cursors.segments();
    if (oldSegments < 1)
        oldSegments = 1;

    // samples per symbol (keep constant when changing segment count)
    float sampPerSeg = (float)selectedSamples.length() / oldSegments;
    if (sampPerSeg < 1)
        sampPerSeg = 1;

    // clamp to file length
    size_t maxSample = mainSampleSource ? mainSampleSource->count() : 0;
    size_t maxAvailable = (maxSample > selectedSamples.minimum)
        ? maxSample - selectedSamples.minimum : 0;

    // max segments that fit
    int maxSegments = (sampPerSeg > 0)
        ? (int)(maxAvailable / sampPerSeg) : segments;
    if (maxSegments < 1)
        maxSegments = 1;

    if (segments > maxSegments) {
        segments = maxSegments;
        emit segmentsChanged(segments);
    }

    selectedSamples.maximum = selectedSamples.minimum +
        (size_t)(segments * sampPerSeg + 0.5f);

    if (maxSample > 0 && selectedSamples.maximum > maxSample)
        selectedSamples.maximum = maxSample;

    cursors.setSegments(segments);
    updateThresholdPlots();
    updateView();
    emitTimeSelection();
}

void PlotView::setOffset(double seconds)
{
    if (!cursorsEnabled || seconds < 0 || sampleRate <= 0)
        return;

    size_t newStart = (size_t)(seconds * sampleRate + 0.5);
    size_t length = selectedSamples.length();
    size_t maxSample = mainSampleSource ? mainSampleSource->count() : 0;

    if (maxSample > 0 && newStart + length > maxSample) {
        if (newStart >= maxSample)
            newStart = (maxSample > length) ? maxSample - length : 0;
    }

    selectedSamples.minimum = newStart;
    selectedSamples.maximum = newStart + length;

    if (maxSample > 0 && selectedSamples.maximum > maxSample)
        selectedSamples.maximum = maxSample;

    updateThresholdPlots();
    updateView();
    emitTimeSelection();
}

void PlotView::setPeriod(double seconds)
{
    if (!cursorsEnabled || seconds <= 0 || sampleRate <= 0)
        return;

    size_t totalSamples = (size_t)(seconds * sampleRate + 0.5);
    if (totalSamples == 0)
        return;

    size_t maxSample = mainSampleSource ? mainSampleSource->count() : 0;

    if (maxSample > 0 && selectedSamples.minimum + totalSamples > maxSample)
        totalSamples = maxSample - selectedSamples.minimum;

    selectedSamples.maximum = selectedSamples.minimum + totalSamples;
    updateThresholdPlots();
    updateView();
    emitTimeSelection();
}

void PlotView::setSymbolRate(double rate)
{
    if (!cursorsEnabled || rate <= 0 || sampleRate <= 0)
        return;

    int segments = cursors.segments();
    double totalTime = segments / rate;
    size_t totalSamples = (size_t)(totalTime * sampleRate + 0.5);

    if (totalSamples == 0)
        return;

    size_t maxSample = mainSampleSource ? mainSampleSource->count() : 0;

    if (maxSample > 0 &&
        selectedSamples.minimum + totalSamples > maxSample) {
        // Clamp and reduce segments to fit
        totalSamples = maxSample - selectedSamples.minimum;
        double sampPerSym = sampleRate / rate;
        if (sampPerSym > 0) {
            segments = (int)(totalSamples / sampPerSym);
            if (segments < 1)
                segments = 1;
            totalSamples = (size_t)(segments * sampPerSym + 0.5);
        }
        cursors.setSegments(segments);
        emit segmentsChanged(segments);
    }

    selectedSamples.maximum = selectedSamples.minimum + totalSamples;
    updateThresholdPlots();
    updateView();
    emitTimeSelection();
}

void PlotView::setLsbFirst(bool lsb)
{
    for (auto &&plot : plots) {
        auto tp = dynamic_cast<ThresholdPlot *>(plot.get());
        if (tp)
            tp->setLsbFirst(lsb);
    }
    viewport()->update();
}

DetectResult PlotView::autoDetectSymbolRate(DemodMode mode)
{
    DetectResult result;
    const char *modeName[] = {"amplitude", "frequency", "phase"};

    if (mainSampleSource == nullptr || sampleRate <= 0) {
        result.status = "No signal loaded";
        return result;
    }

    range_t<size_t> range = cursorsEnabled ? selectedSamples : viewRange;

    if (range.length() < 16) {
        result.status = "Selection too short";
        return result;
    }

    size_t len = std::min(range.length(), (size_t)65536);

    /* always use tuner output */
    auto tunerSrc = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(
        spectrogramPlot->output());
    spectrogramPlot->tunerFullUpdate();

    /*
     * Get demodulated float samples + amplitude for gating.
     *
     * Amplitude mode: abs() on tuner IQ (proven, do not change).
     *
     * Frequency/Phase mode:
     *   1. Try existing derived plot (user sees clean data)
     *   2. Fallback: create temporary FrequencyDemod/PhaseDemod
     *      from tuner output (uses liquid-dsp, same as plots)
     */
    std::vector<float> demod(len);
    std::vector<bool> signalPresent(len, true);
    bool gotDemod = false;
    QString demodSource;

    /* get tuner IQ samples (needed for amplitude in all modes) */
    auto iqSrc = tunerSrc ? tunerSrc.get() : mainSampleSource;
    auto iqSamples = iqSrc->getSamples(range.minimum, len);

    if (!iqSamples) {
        result.status = "Failed to read samples";
        return result;
    }

    if (mode == DemodAmplitude) {
        for (size_t i = 0; i < len; i++)
            demod[i] = std::abs(iqSamples[i]);
        gotDemod = true;
        demodSource = "tuner amplitude";
    } else {
        /* try existing derived plot first */
        const char *plotName = (mode == DemodFrequency)
            ? "frequency plot" : "phase plot";

        for (size_t pi = 0; pi < derivedPlotInfos.size(); pi++) {
            if (pi + 1 >= plots.size())
                break;
            if (derivedPlotInfos[pi].typeName == plotName) {
                auto floatSrc = std::dynamic_pointer_cast<
                    SampleSource<float>>(plots[pi + 1]->output());
                if (!floatSrc)
                    continue;
                auto samples = floatSrc->getSamples(range.minimum, len);
                if (!samples)
                    continue;
                for (size_t i = 0; i < len; i++)
                    demod[i] = samples[i];
                gotDemod = true;
                demodSource = QString("%1 (existing plot)").arg(plotName);
                break;
            }
        }

        /* fallback: create temporary demodulator */
        if (!gotDemod && tunerSrc) {
            std::shared_ptr<SampleSource<float>> tmpDemod;

            if (mode == DemodFrequency)
                tmpDemod = std::make_shared<FrequencyDemod>(tunerSrc);
            else
                tmpDemod = std::make_shared<PhaseDemod>(tunerSrc);

            auto samples = tmpDemod->getSamples(range.minimum, len);
            if (samples) {
                for (size_t i = 0; i < len; i++)
                    demod[i] = samples[i];
                gotDemod = true;
                demodSource = QString("%1 (from tuner)")
                    .arg(modeName[mode]);
            }
        }

        if (!gotDemod) {
            result.status = QString("No %1 data available")
                .arg(modeName[mode]);
            return result;
        }

        /* amplitude gate: ignore frequency/phase where no signal */
        std::vector<float> amp(len);
        for (size_t i = 0; i < len; i++)
            amp[i] = std::abs(iqSamples[i]);

        std::vector<float> ampSorted(amp.begin(), amp.end());
        std::nth_element(ampSorted.begin(),
                         ampSorted.begin() + ampSorted.size() / 4,
                         ampSorted.end());
        float ap25 = ampSorted[ampSorted.size() / 4];
        std::nth_element(ampSorted.begin(),
                         ampSorted.begin() + ampSorted.size() * 3 / 4,
                         ampSorted.end());
        float ap75 = ampSorted[ampSorted.size() * 3 / 4];
        float ampThresh = (ap25 + ap75) / 2.0f;

        for (size_t i = 0; i < len; i++)
            signalPresent[i] = amp[i] > ampThresh;
    }

    /*
     * Detection pipeline: smooth -> Schmitt trigger -> run lengths
     */
    size_t smoothWin = std::max(len / 500, (size_t)3);
    std::vector<float> smooth(len);
    double sacc = 0;

    for (size_t i = 0; i < len; i++) {
        sacc += demod[i];
        if (i >= smoothWin)
            sacc -= demod[i - smoothWin];
        size_t w = std::min(i + 1, smoothWin);
        smooth[i] = (float)(sacc / w);
    }

    /* percentiles from signal-present samples only */
    std::vector<float> signalValues;
    for (size_t i = 0; i < len; i++) {
        if (signalPresent[i])
            signalValues.push_back(smooth[i]);
    }

    if (signalValues.size() < 16) {
        result.status = QString("Not enough signal samples (%1 mode)")
            .arg(modeName[mode]);
        return result;
    }

    std::sort(signalValues.begin(), signalValues.end());
    float p10 = signalValues[signalValues.size() / 10];
    float p90 = signalValues[signalValues.size() * 9 / 10];

    if (p90 <= p10) {
        result.status = QString("No level variation detected (%1 mode)")
            .arg(modeName[mode]);
        return result;
    }

    float highThresh = p10 + (p90 - p10) * 0.6f;
    float lowThresh = p10 + (p90 - p10) * 0.4f;

    /* Schmitt trigger + run lengths (gated) */
    size_t minRun = std::max(smoothWin, (size_t)4);
    std::vector<size_t> runLengths;
    bool state = smooth[0] > highThresh;
    size_t runStart = 0;
    bool inSignal = signalPresent[0];
    int transitionCount = 0;

    for (size_t i = 1; i < len; i++) {
        if (signalPresent[i] != inSignal) {
            if (inSignal) {
                size_t runLen = i - runStart;
                if (runLen >= minRun)
                    runLengths.push_back(runLen);
            }
            runStart = i;
            inSignal = signalPresent[i];
            if (inSignal)
                state = smooth[i] > highThresh;
            continue;
        }

        if (!inSignal)
            continue;

        bool transition = false;

        if (state && smooth[i] < lowThresh) {
            state = false;
            transition = true;
        } else if (!state && smooth[i] > highThresh) {
            state = true;
            transition = true;
        }

        if (transition) {
            transitionCount++;
            size_t runLen = i - runStart;
            if (runLen >= minRun)
                runLengths.push_back(runLen);
            runStart = i;
        }
    }

    if (runLengths.size() < 2) {
        result.status = QString("Only %1 transitions found (%2)")
            .arg(transitionCount).arg(demodSource);
        return result;
    }

    /* shortest cluster = symbol period */
    std::sort(runLengths.begin(), runLengths.end());
    size_t shortest = runLengths[0];
    size_t clusterEnd = 0;

    for (size_t i = 0; i < runLengths.size(); i++) {
        if (runLengths[i] <= shortest * 1.5)
            clusterEnd = i;
        else
            break;
    }

    size_t symbolPeriod = runLengths[clusterEnd / 2];

    if (symbolPeriod == 0) {
        result.status = "Symbol period is zero";
        return result;
    }

    result.rate = sampleRate / symbolPeriod;
    result.transitions = transitionCount;
    result.status = QString("%1 Bd (%2 transitions, %3)")
        .arg(result.rate, 0, 'f', 2)
        .arg(transitionCount)
        .arg(demodSource);

    return result;
}


void PlotView::saveViewPosition(double &timeSec, double &freqHz)
{
    timeSec = 0;
    freqHz = 0;
    if (sampleRate > 0) {
        size_t centerSample = columnToSample(horizontalScrollBar()->value())
            + columnToSample(width()) / 2;
        timeSec = (double)centerSample / sampleRate;
    }
    if (spectrogramPlot && spectrogramPlot->getFFTSize() > 0 && sampleRate > 0) {
        int vCenter = verticalScrollBar()->value() + viewport()->height() / 2;
        freqHz = (0.5 - (double)vCenter / spectrogramPlot->getFFTSize())
                 * sampleRate;
    }
}

void PlotView::restoreViewPosition(double timeSec, double freqHz)
{
    if (sampleRate <= 0)
        return;

    /* restore horizontal: center on saved time */
    size_t centerSample = (size_t)(timeSec * sampleRate);
    int hVal = sampleToColumn(centerSample) - width() / 2;
    horizontalScrollBar()->setValue(std::max(0, hVal));

    /* restore vertical: convert freq back to bin in new FFT size */
    if (spectrogramPlot && spectrogramPlot->getFFTSize() > 0) {
        int newFFT = spectrogramPlot->getFFTSize();
        int centerBin = (int)((0.5 - freqHz / sampleRate) * newFFT);
        int vVal = centerBin - viewport()->height() / 2;
        verticalScrollBar()->setValue(
            std::max(0, std::min(vVal, verticalScrollBar()->maximum())));
    }

    updateViewRange(false);
}

void PlotView::setFFTAndZoom(int size, int zoom)
{
    auto oldSamplesPerColumn = samplesPerColumn();

    double timeSec = 0, freqHz = 0;
    saveViewPosition(timeSec, freqHz);

    if (size != fftSize) {
        fftSize = size;
        if (spectrogramPlot != nullptr)
            spectrogramPlot->setFFTSize(size);
    }

    zoomLevel = zoom;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setZoomLevel(zoom);

    horizontalScrollBar()->setSingleStep(10);
    horizontalScrollBar()->setPageStep(100);

    updateView(false, samplesPerColumn() < oldSamplesPerColumn);

    restoreViewPosition(timeSec, freqHz);
}

void PlotView::setPowerMin(int power)
{
    powerMin = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMin(power);
    updateView();
}

void PlotView::setZeroPad(int level)
{
    int factor = 1 << level;
    if (spectrogramPlot == nullptr)
        return;

    double timeSec = 0, freqHz = 0;
    saveViewPosition(timeSec, freqHz);

    spectrogramPlot->setZeroPad(factor);
    updateView(true);

    restoreViewPosition(timeSec, freqHz);
}

void PlotView::setZoomY(int level)
{
    int zoomY = 1 << level;
    if (spectrogramPlot == nullptr)
        return;

    double timeSec = 0, freqHz = 0;
    saveViewPosition(timeSec, freqHz);

    spectrogramPlot->setZoomY(zoomY);
    updateView();

    restoreViewPosition(timeSec, freqHz);
}

void PlotView::setTunerVisible(bool visible)
{
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setTunerVisible(visible);
    viewport()->update();
}

void PlotView::setCropToTuner(bool enabled)
{
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableMaskOutOfBand(enabled);
    updateView();

    /* when uncropping, scroll vertically to center on tuner */
    if (!enabled && spectrogramPlot != nullptr) {
        int tunerY = spectrogramPlot->tunerCentre();
        int viewH = viewport()->height();
        verticalScrollBar()->setValue(
            std::max(0, tunerY - viewH / 2));
    }
}

void PlotView::setAveraging(int count)
{
    int factor = 1 << count;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setAveraging(factor);
}


void PlotView::jumpToBookmark(double timeSec, double freqHz)
{
    jumpToTime(timeSec);
    jumpToFreq(freqHz);
}

void PlotView::jumpToTime(double timeSec)
{
    if (sampleRate <= 0) return;
    size_t sample = (size_t)(timeSec * sampleRate);
    horizontalScrollBar()->setValue(sampleToColumn(sample));
    updateView();
}

void PlotView::jumpToFreq(double freqHz)
{
    if (!spectrogramPlot || sampleRate <= 0) return;
    int fft = spectrogramPlot->getFFTSize();
    if (fft <= 0) return;

    /* frequency -> FFT bin */
    int bin = (int)((0.5 - freqHz / sampleRate) * fft);

    /* scroll so this bin is at the center of the viewport */
    int viewH = viewport()->height();
    verticalScrollBar()->setValue(std::max(0, bin - viewH / 2));
    updateView();
}

void PlotView::setPowerMax(int power)
{
    powerMax = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMax(power);
    updateView();
}

void PlotView::paintEvent(QPaintEvent *)
{
    if (mainSampleSource == nullptr) return;

    QRect rect = QRect(0, 0, width(), height());
    QPainter painter(viewport());
    painter.fillRect(rect, Qt::black);

#define PLOT_LAYER(paintFunc)                                                   \
    {                                                                           \
        int y = -verticalScrollBar()->value();                                  \
        for (auto&& plot : plots) {                                             \
            QRect rect = QRect(0, y, width(), plot->height());                  \
            plot->paintFunc(painter, rect, viewRange);                          \
            y += plot->height();                                                \
        }                                                                       \
    }

    PLOT_LAYER(paintBack);
    PLOT_LAYER(paintMid);
    PLOT_LAYER(paintFront);

    if (cursorsEnabled)
        cursors.paintFront(painter, rect, viewRange);

    if (timeScaleEnabled) {
        paintTimeScale(painter, rect, viewRange);
    }

#undef PLOT_LAYER
}

void PlotView::paintTimeScale(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (sampleRate <= 0)
        return;

    float startTime = (float)sampleRange.minimum / sampleRate;
    float stopTime = (float)sampleRange.maximum / sampleRate;
    float duration = stopTime - startTime;

    if (duration <= 0)
        return;

    painter.save();

    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());

    int tickWidth = 80;
    int maxTicks = rect.width() / tickWidth;

    double durationPerTick = 10 * pow(10, floor(log(duration / maxTicks) / log(10)));

    double firstTick = int(startTime / durationPerTick) * durationPerTick;

    double tick = firstTick;

    if (durationPerTick <= 0 || maxTicks <= 0) {
        painter.restore();
        return;
    }

    int botY = viewport()->height() - 30;

    while (tick <= stopTime) {
        if (tick < 0) { tick += durationPerTick; continue; }

        size_t tickSample = (size_t)(tick * sampleRate);
        if (tickSample < sampleRange.minimum) { tick += durationPerTick; continue; }
        int tickLine = sampleToColumn(tickSample - sampleRange.minimum);

        char buf[128];
        snprintf(buf, sizeof(buf), "%.06f", tick);

        /* top */
        painter.drawLine(tickLine, 0, tickLine, 30);
        painter.drawText(tickLine + 2, 25, buf);

        /* bottom */
        painter.drawLine(tickLine, botY, tickLine, botY + 30);
        painter.drawText(tickLine + 2, botY + 25, buf);

        tick += durationPerTick;
    }

    // Draw small ticks
    durationPerTick /= 10;
    if (durationPerTick <= 0) { painter.restore(); return; }
    firstTick = int(startTime / durationPerTick) * durationPerTick;
    tick = firstTick;
    while (tick <= stopTime) {
        if (tick < 0) { tick += durationPerTick; continue; }

        size_t tickSample = (size_t)(tick * sampleRate);
        if (tickSample < sampleRange.minimum) { tick += durationPerTick; continue; }
        int tickLine = sampleToColumn(tickSample - sampleRange.minimum);

        painter.drawLine(tickLine, 0, tickLine, 10);
        painter.drawLine(tickLine, botY, tickLine, botY + 10);
        tick += durationPerTick;
    }

    painter.restore();
}

int PlotView::plotsHeight()
{
    int height = 0;
    for (auto&& plot : plots) {
        height += plot->height();
    }
    return height;
}

void PlotView::resizeEvent(QResizeEvent *)
{
    updateView();
}

size_t PlotView::samplesPerColumn()
{
    if (zoomLevel <= 0)
        return fftSize;
    return fftSize / zoomLevel;
}

void PlotView::scrollContentsBy(int, int)
{
    updateView();
}

void PlotView::showEvent(QShowEvent *)
{
    // Intentionally left blank. See #171
}

void PlotView::updateViewRange(bool reCenter)
{
    if (mainSampleSource == nullptr)
        return;

    // Update current view -- don't clamp end to file size so
    // TracePlots use the same samplesPerColumn as the spectrogram
    auto start = columnToSample(horizontalScrollBar()->value());
    size_t viewLen = columnToSample(width());

    viewRange = {start, start + viewLen};

    // Adjust time offset to zoom around central sample
    if (reCenter) {
        horizontalScrollBar()->setValue(
            sampleToColumn(zoomSample) - zoomPos
        );
    }
    zoomSample = viewRange.minimum + viewRange.length() / 2;
    zoomPos = width() / 2;

    /* emit current position for View panel */
    if (sampleRate > 0) {
        double timeSec = (double)start / sampleRate;
        double freqHz = 0;
        if (spectrogramPlot && spectrogramPlot->getFFTSize() > 0) {
            /* frequency at center of visible spectrogram */
            int vScroll = verticalScrollBar()->value();
            int plotH = spectrogramPlot->height();
            int centerBin = vScroll + std::min(viewport()->height(), plotH) / 2;
            freqHz = (0.5 - (double)centerBin / spectrogramPlot->getFFTSize())
                     * sampleRate;
        }
        emit viewPositionChanged(timeSec, freqHz);
    }
}

void PlotView::updateView(bool reCenter, bool expanding)
{
    if (!expanding) {
        updateViewRange(reCenter);
    }
    horizontalScrollBar()->setMaximum(std::max(0, sampleToColumn(mainSampleSource->count()) - width()));
    verticalScrollBar()->setMaximum(std::max(0, plotsHeight() - viewport()->height()));
    if (expanding) {
        updateViewRange(reCenter);
    }

    // Update cursors
    range_t<int> newSelection = {
        sampleToColumn(selectedSamples.minimum) - horizontalScrollBar()->value(),
        sampleToColumn(selectedSamples.maximum) - horizontalScrollBar()->value()
    };
    cursors.setSelection(newSelection);

    // Re-paint
    viewport()->update();
}

void PlotView::setSampleRate(double rate)
{
    sampleRate = rate;

    if (spectrogramPlot != nullptr)
        spectrogramPlot->setSampleRate(rate);

    emitTimeSelection();
}

void PlotView::enableScales(bool enabled)
{
    timeScaleEnabled = enabled;

    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableScales(enabled);

    viewport()->update();
}

void PlotView::enableAnnotations(bool enabled)
{
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableAnnotations(enabled);

    viewport()->update();
}

void PlotView::enableAnnotationCommentsTooltips(bool enabled)
{
    annotationCommentsEnabled = enabled;

    viewport()->update();
}

int PlotView::sampleToColumn(size_t sample)
{
    return sample / samplesPerColumn();
}

size_t PlotView::columnToSample(int col)
{
    if (col <= 0)
        return 0;
    return (size_t)col * samplesPerColumn();
}

int PlotView::getTunerCentre()
{
    if (spectrogramPlot)
        return spectrogramPlot->tunerCentre();
    return 0;
}

int PlotView::getTunerDeviation()
{
    if (spectrogramPlot)
        return spectrogramPlot->tunerDeviation();
    return 0;
}

void PlotView::setTunerPosition(int centre, int deviation)
{
    if (spectrogramPlot) {
        spectrogramPlot->setTunerDeviation(deviation);
        spectrogramPlot->setTunerCentre(centre);
        spectrogramPlot->tunerFullUpdate();
    }
}

void PlotView::setTunerCentreHz(double hz)
{
    if (!spectrogramPlot || sampleRate <= 0)
        return;

    int fft = spectrogramPlot->getFFTSize();
    if (fft <= 0)
        return;

    /* CF in Hz -> FFT bin: centre = (0.5 - cf/sampleRate) * fftSize */
    int bin = (int)((0.5 - hz / sampleRate) * fft + 0.5);
    spectrogramPlot->setTunerCentre(bin);
    spectrogramPlot->tunerFullUpdate();
    updateView();
}

void PlotView::setTunerBandwidthHz(double hz)
{
    if (!spectrogramPlot || sampleRate <= 0)
        return;

    int fft = spectrogramPlot->getFFTSize();
    if (fft <= 0)
        return;

    /* BW in Hz -> deviation in bins: dev = bw/2 / sampleRate * fftSize */
    int dev = (int)(hz / 2.0 / sampleRate * fft + 0.5);
    if (dev < 1)
        dev = 1;
    spectrogramPlot->setTunerDeviation(dev);
    spectrogramPlot->tunerFullUpdate();
    updateView();
}

QJsonArray PlotView::getDerivedPlotsState()
{
    QJsonArray arr;
    for (auto &info : derivedPlotInfos) {
        QJsonObject obj;
        obj["parentIndex"] = info.parentIndex;
        obj["type"] = info.typeName;
        arr.append(obj);
    }
    return arr;
}

void PlotView::restoreSessionPlots(const QJsonArray &plotsArray)
{
    /* clear pixmap cache first -- this ensures no TracePlot
     * tile render tasks are queued (they only start on cache miss
     * during paint, which we're not doing here) */
    QPixmapCache::clear();

    /* wait for any in-flight QtConcurrent tasks to finish
     * before destroying the plots they reference */
    QThreadPool::globalInstance()->waitForDone();

    /* now safe to remove all derived plots */
    while (plots.size() > 1)
        plots.pop_back();
    derivedPlotInfos.clear();

    for (auto val : plotsArray) {
        QJsonObject obj = val.toObject();
        int parentIdx = obj["parentIndex"].toInt();
        QString typeName = obj["type"].toString();

        if (parentIdx < 0 || parentIdx >= (int)plots.size())
            continue;

        auto src = plots[parentIdx]->output();
        if (!src)
            continue;
        auto compatible = as_range(Plots::plots.equal_range(src->sampleType()));
        for (auto p : compatible) {
            if (typeName == p.second.name) {
                addPlot(p.second.creator(src));
                derivedPlotInfos.push_back({parentIdx, typeName});
                break;
            }
        }
    }
}

void PlotView::setSelectedSamples(range_t<size_t> samples)
{
    selectedSamples = samples;
    updateThresholdPlots();
    updateView();
    emitTimeSelection();
}

void PlotView::setScrollPosition(int hValue, int vValue)
{
    /* recalculate scrollbar maximums based on current plots */
    horizontalScrollBar()->setMaximum(
        std::max(0, sampleToColumn(mainSampleSource->count()) - width()));
    verticalScrollBar()->setMaximum(
        std::max(0, plotsHeight() - viewport()->height()));

    horizontalScrollBar()->setValue(hValue);
    verticalScrollBar()->setValue(vValue);
    updateViewRange(false);
    viewport()->update();
}
