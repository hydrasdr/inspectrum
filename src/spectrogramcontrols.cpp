/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *  Copyright (C) 2015, Jared Boone <jared@sharebrained.com>
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

#include "spectrogramcontrols.h"
#include <climits>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QFileDialog>
#include <QSettings>
#include <QLabel>
#include <cmath>
#include "util.h"

SpectrogramControls::SpectrogramControls(const QString & title, QWidget * parent)
    : QDockWidget::QDockWidget(title, parent)
{
    widget = new QWidget(this);
    layout = new QFormLayout(widget);

    fileOpenButton = new QPushButton("Open file/session...", widget);
    saveSessionButton = new QPushButton("Save session...", widget);
    QHBoxLayout *openLayout = new QHBoxLayout();
    openLayout->setContentsMargins(0, 0, 0, 0);
    openLayout->addWidget(fileOpenButton, 1);
    openLayout->addWidget(saveSessionButton);
    layout->addRow(openLayout);

    sampleRate = new QLineEdit();
    auto double_validator = new QDoubleValidator(this);
    double_validator->setBottom(0.0);
    sampleRate->setValidator(double_validator);
    layout->addRow(new QLabel(tr("Sample rate:")), sampleRate);

    // View position & bookmarks
    layout->addRow(new QLabel(tr("<b>View</b>")));

    viewPosXEdit = new QLineEdit("0");
    viewPosXEdit->setValidator(new QDoubleValidator(0, 1e9, 6, this));
    layout->addRow(new QLabel(tr("Pos X (s):")), viewPosXEdit);

    viewPosYEdit = new QLineEdit("0");
    layout->addRow(new QLabel(tr("Pos Y:")), viewPosYEdit);

    bookmarkCombo = new QComboBox(widget);
    bookmarkCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *addBtn = new QPushButton("+", widget);
    auto *rmBtn = new QPushButton("-", widget);
    auto *editBtn = new QPushButton("U", widget);
    addBtn->setMaximumWidth(24);
    rmBtn->setMaximumWidth(24);
    editBtn->setMaximumWidth(24);

    QHBoxLayout *bmLayout = new QHBoxLayout();
    bmLayout->setContentsMargins(0, 0, 0, 0);
    bmLayout->addWidget(bookmarkCombo, 1);
    bmLayout->addWidget(addBtn);
    bmLayout->addWidget(editBtn);
    bmLayout->addWidget(rmBtn);
    layout->addRow(new QLabel(tr("Bookmarks:")), bmLayout);

    lsbFirstCheckBox = new QCheckBox(tr("LSB first"), widget);
    layout->addRow(new QLabel(tr("Bit order:")), lsbFirstCheckBox);

    connect(addBtn, &QPushButton::clicked, this, &SpectrogramControls::addBookmark);
    connect(rmBtn, &QPushButton::clicked, this, &SpectrogramControls::removeBookmark);
    connect(editBtn, &QPushButton::clicked, this, &SpectrogramControls::editBookmark);
    connect(bookmarkCombo, QOverload<int>::of(&QComboBox::activated),
            this, &SpectrogramControls::onBookmarkActivated);
    connect(viewPosXEdit, &QLineEdit::returnPressed, this, [this]() {
        viewPosXEdit->clearFocus();
    });
    connect(viewPosXEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok;
        double t = viewPosXEdit->text().toDouble(&ok);
        if (ok && t >= 0)
            emit viewPosXEdited(t);
    });
    connect(viewPosYEdit, &QLineEdit::returnPressed, this, [this]() {
        viewPosYEdit->clearFocus();
    });
    connect(viewPosYEdit, &QLineEdit::editingFinished, this, [this]() {
        double f;
        if (parseSIValue(viewPosYEdit->text().toStdString(), f))
            emit viewPosYEdited(f);
    });

    // Spectrogram settings
    layout->addRow(new QLabel(tr("<b>Spectrogram</b>")));

    fftSizeSlider = new QSlider(Qt::Horizontal, widget);
    fftSizeSlider->setRange(2, 14);
    fftSizeSlider->setPageStep(1);
    fftSizeLabel = new QLabel(tr("FFT size:"));
    layout->addRow(fftSizeLabel, fftSizeSlider);
    connect(fftSizeSlider, &QSlider::valueChanged, this, [this](int v) {
        fftSizeLabel->setText(QString("FFT size (%1):").arg(1 << v));
    });

    zoomLevelSlider = new QSlider(Qt::Horizontal, widget);
    zoomLevelSlider->setRange(0, 10);
    zoomLevelSlider->setPageStep(1);
    zoomLabel = new QLabel(tr("Zoom:"));
    layout->addRow(zoomLabel, zoomLevelSlider);
    connect(zoomLevelSlider, &QSlider::valueChanged, this, [this](int v) {
        zoomLabel->setText(QString("Zoom (%1x):").arg(1 << v));
    });

    zeroPadSlider = new QSlider(Qt::Horizontal, widget);
    zeroPadSlider->setRange(0, 3);
    zeroPadSlider->setPageStep(1);
    zeroPadSlider->setValue(0);
    zeroPadLabel = new QLabel(tr("Zero-pad:"));
    layout->addRow(zeroPadLabel, zeroPadSlider);
    connect(zeroPadSlider, &QSlider::valueChanged, this, [this](int v) {
        zeroPadLabel->setText(QString("Zero-pad (%1x):").arg(1 << v));
    });

    zoomYSlider = new QSlider(Qt::Horizontal, widget);
    zoomYSlider->setRange(0, 3);
    zoomYSlider->setPageStep(1);
    zoomYSlider->setValue(0);
    zoomYLabel = new QLabel(tr("Zoom Y:"));
    layout->addRow(zoomYLabel, zoomYSlider);
    connect(zoomYSlider, &QSlider::valueChanged, this, [this](int v) {
        zoomYLabel->setText(QString("Zoom Y (%1x):").arg(1 << v));
    });

    avgSlider = new QSlider(Qt::Horizontal, widget);
    avgSlider->setRange(0, 5);  /* 2^0=1x to 2^5=32x */
    avgSlider->setPageStep(1);
    avgSlider->setValue(0);
    avgLabel = new QLabel(tr("Averaging (1x):"));
    layout->addRow(avgLabel, avgSlider);
    connect(avgSlider, &QSlider::valueChanged, this, [this](int v) {
        avgLabel->setText(QString("Averaging (%1x):").arg(1 << v));
        emit avgChanged(v);
    });

    powerMaxSlider = new QSlider(Qt::Horizontal, widget);
    powerMaxSlider->setRange(-150, 20);
    powerMaxLabel = new QLabel(tr("Power max (dBFS):"));
    layout->addRow(powerMaxLabel, powerMaxSlider);
    connect(powerMaxSlider, &QSlider::valueChanged, this, [this](int v) {
        powerMaxLabel->setText(QString("Power max (%1 dBFS):").arg(v));
    });

    powerMinSlider = new QSlider(Qt::Horizontal, widget);
    powerMinSlider->setRange(-150, 20);
    powerMinLabel = new QLabel(tr("Power min (dBFS):"));
    layout->addRow(powerMinLabel, powerMinSlider);
    connect(powerMinSlider, &QSlider::valueChanged, this, [this](int v) {
        powerMinLabel->setText(QString("Power min (%1 dBFS):").arg(v));
    });

    scalesCheckBox = new QCheckBox(widget);
    scalesCheckBox->setCheckState(Qt::Checked);
    layout->addRow(new QLabel(tr("Scales:")), scalesCheckBox);

    renderTimeLabel = new QLabel("- ms");
    renderTimeLabel->setStyleSheet("QLabel { color: #888; font-size: 10px; }");
    renderResetButton = new QPushButton("Reset", widget);
    renderResetButton->setMaximumWidth(50);
    renderResetButton->setStyleSheet("font-size: 10px;");
    QHBoxLayout *renderLayout = new QHBoxLayout();
    renderLayout->addWidget(renderTimeLabel);
    renderLayout->addWidget(renderResetButton);
    layout->addRow(new QLabel(tr("Render:")), renderLayout);
    connect(renderResetButton, &QPushButton::clicked,
            this, &SpectrogramControls::resetRenderStats);

    // Tuner info
    layout->addRow(new QLabel(tr("<b>Tuner</b>")));

    tunerCentreEdit = new QLineEdit();
    layout->addRow(new QLabel(tr("Centre freq:")), tunerCentreEdit);

    tunerBandwidthEdit = new QLineEdit();
    layout->addRow(new QLabel(tr("Bandwidth:")), tunerBandwidthEdit);

    maskOutOfBandCheckBox = new QCheckBox(widget);
    tunerVisibleCheckBox = new QCheckBox(tr("Show"), widget);
    tunerVisibleCheckBox->setChecked(true);
    QHBoxLayout *cropVisLayout = new QHBoxLayout();
    cropVisLayout->setContentsMargins(0, 0, 0, 0);
    cropVisLayout->addWidget(maskOutOfBandCheckBox);
    cropVisLayout->addWidget(tunerVisibleCheckBox);
    layout->addRow(new QLabel(tr("Crop to tuner:")), cropVisLayout);
    connect(tunerVisibleCheckBox, &QCheckBox::toggled,
            this, &SpectrogramControls::tunerVisibleChanged);

    // Time selection settings
    layout->addRow(new QLabel(tr("<b>Time selection</b>")));

    cursorsCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Enable cursors:")), cursorsCheckBox);

    cursorGridSlider = new QSlider(Qt::Horizontal, widget);
    cursorGridSlider->setRange(0, 255);
    cursorGridSlider->setValue(80);
    gridOpacityLabel = new QLabel(tr("Grid opacity:"));
    layout->addRow(gridOpacityLabel, cursorGridSlider);
    connect(cursorGridSlider, &QSlider::valueChanged, this, [this](int v) {
        gridOpacityLabel->setText(QString("Grid opacity (%1):").arg(v));
    });

    offsetEdit = new QLineEdit();
    auto offsetValidator = new QDoubleValidator(this);
    offsetValidator->setBottom(0.0);
    offsetEdit->setValidator(offsetValidator);
    layout->addRow(new QLabel(tr("Offset (s):")), offsetEdit);

    periodEdit = new QLineEdit();
    auto periodValidator = new QDoubleValidator(this);
    periodValidator->setBottom(0.0);
    periodEdit->setValidator(periodValidator);
    layout->addRow(new QLabel(tr("Period (s):")), periodEdit);

    cursorSymbolsSpinBox = new QSpinBox();
    cursorSymbolsSpinBox->setMinimum(1);
    cursorSymbolsSpinBox->setMaximum(99999);
    layout->addRow(new QLabel(tr("Symbols:")), cursorSymbolsSpinBox);

    symbolRateEdit = new QLineEdit();
    layout->addRow(new QLabel(tr("Symbol rate:")), symbolRateEdit);

    rateLabel = new QLabel();
    layout->addRow(new QLabel(tr("Rate:")), rateLabel);

    symbolPeriodLabel = new QLabel();
    layout->addRow(new QLabel(tr("Symbol period:")), symbolPeriodLabel);

    autoDetectButton = new QPushButton("Auto detect rate (ASK/OOK)", widget);
    autoDetectButton->setToolTip("Detect symbol rate from amplitude.\n"
                                 "Requires:\n"
                                 "1. Enable cursors\n"
                                 "2. Add amplitude plot (right-click on spectrogram)\n"
                                 "3. Tuner well positioned on signal\n"
                                 "4. Set Symbols >= 1\n"
                                 "5. Period long enough to cover lot of symbols");
    layout->addRow(autoDetectButton);

    detectStatusLabel = new QLabel(widget);
    detectStatusLabel->setWordWrap(true);
    detectStatusLabel->setStyleSheet("QLabel { color: #aaa; font-size: 10px; }");
    layout->addRow(detectStatusLabel);

    // SigMF selection settings
    layout->addRow(new QLabel(tr("<b>SigMF Control</b>")));

    annosCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Display Annotations:")), annosCheckBox);
    commentsCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Display annotation\ncomments tooltips:")), commentsCheckBox);

    widget->setLayout(layout);
    setWidget(widget);

    connect(fftSizeSlider, &QSlider::valueChanged, this, &SpectrogramControls::fftSizeChanged);
    connect(zoomLevelSlider, &QSlider::valueChanged, this, &SpectrogramControls::zoomLevelChanged);
    connect(fileOpenButton, &QPushButton::clicked, this, &SpectrogramControls::fileOpenButtonClicked);
    connect(cursorsCheckBox, &QCheckBox::stateChanged, this, &SpectrogramControls::cursorsStateChanged);
    connect(powerMinSlider, &QSlider::valueChanged, this, &SpectrogramControls::powerMinChanged);
    connect(powerMaxSlider, &QSlider::valueChanged, this, &SpectrogramControls::powerMaxChanged);
    connect(saveSessionButton, &QPushButton::clicked, this, &SpectrogramControls::saveSession);
    connect(autoDetectButton, &QPushButton::clicked, this, &SpectrogramControls::autoDetectRate);
    connect(lsbFirstCheckBox, &QCheckBox::toggled, this, &SpectrogramControls::lsbFirstChanged);
    connect(symbolRateEdit, &QLineEdit::returnPressed, this, [this]() {
        symbolRateEdit->clearFocus();
    });
    connect(symbolRateEdit, &QLineEdit::editingFinished, this, [this]() {
        double rate;
        if (parseSIValue(symbolRateEdit->text().toStdString(), rate) && rate > 0)
            emit symbolRateChanged(rate);
    });
    connect(periodEdit, &QLineEdit::returnPressed, this, [this]() {
        periodEdit->clearFocus();
    });
    connect(periodEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok;
        double period = periodEdit->text().toDouble(&ok);
        if (ok && period > 0)
            emit periodChanged(period);
    });
    connect(offsetEdit, &QLineEdit::returnPressed, this, [this]() {
        offsetEdit->clearFocus();
    });
    connect(offsetEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok;
        double offset = offsetEdit->text().toDouble(&ok);
        if (ok && offset >= 0)
            emit offsetChanged(offset);
    });
    connect(tunerCentreEdit, &QLineEdit::returnPressed, this, [this]() {
        double hz;
        if (parseSIValue(tunerCentreEdit->text().toStdString(), hz)) {
            tunerCentreEdit->clearFocus();
            emit tunerCentreEdited(hz);
        }
    });
    connect(tunerCentreEdit, &QLineEdit::editingFinished, this, [this]() {
        double hz;
        if (parseSIValue(tunerCentreEdit->text().toStdString(), hz))
            emit tunerCentreEdited(hz);
    });
    connect(tunerBandwidthEdit, &QLineEdit::returnPressed, this, [this]() {
        double hz;
        if (parseSIValue(tunerBandwidthEdit->text().toStdString(), hz) && hz > 0) {
            tunerBandwidthEdit->clearFocus();
            emit tunerBandwidthEdited(hz);
        }
    });
    connect(tunerBandwidthEdit, &QLineEdit::editingFinished, this, [this]() {
        double hz;
        if (parseSIValue(tunerBandwidthEdit->text().toStdString(), hz) && hz > 0)
            emit tunerBandwidthEdited(hz);
    });

    /* debounced settings persistence: flush 500ms after last change */
    settingsSaveTimer.setSingleShot(true);
    connect(&settingsSaveTimer, &QTimer::timeout, this, &SpectrogramControls::flushSettings);
}

void SpectrogramControls::clearCursorLabels()
{
    offsetEdit->setText("");
    periodEdit->setText("");
    rateLabel->setText("");
    symbolPeriodLabel->setText("");
    symbolRateEdit->setText("");
}

void SpectrogramControls::cursorsStateChanged(int state)
{
    if (state == Qt::Unchecked) {
        clearCursorLabels();
    }
}

void SpectrogramControls::setDefaults()
{
    loadBookmarks();
    fftOrZoomChanged();

    cursorsCheckBox->setCheckState(Qt::Unchecked);
    cursorSymbolsSpinBox->setValue(1);

    annosCheckBox->setCheckState(Qt::Checked);
    commentsCheckBox->setCheckState(Qt::Checked);

    QSettings settings;
    int savedSampleRate = settings.value("SampleRate", 8000000).toInt();
    sampleRate->setText(QString::fromStdString(
        formatSIValueSigned(savedSampleRate, "Hz")));
    fftSizeSlider->setValue(settings.value("FFTSize", 9).toInt());
    powerMaxSlider->setValue(settings.value("PowerMax", 0).toInt());
    powerMinSlider->setValue(settings.value("PowerMin", -100).toInt());
    zoomLevelSlider->setValue(settings.value("ZoomLevel", 0).toInt());

    emit fftSizeSlider->valueChanged(fftSizeSlider->value());
    emit zoomLevelSlider->valueChanged(zoomLevelSlider->value());
    emit zeroPadSlider->valueChanged(zeroPadSlider->value());
    emit zoomYSlider->valueChanged(zoomYSlider->value());
    emit powerMaxSlider->valueChanged(powerMaxSlider->value());
    emit powerMinSlider->valueChanged(powerMinSlider->value());
    emit cursorGridSlider->valueChanged(cursorGridSlider->value());
}

void SpectrogramControls::fftOrZoomChanged(void)
{
    int fftSize = 1 << fftSizeSlider->value();
    int zoomLevel = std::min(fftSize, 1 << zoomLevelSlider->value());
    emit fftOrZoomChanged(fftSize, zoomLevel);
}

void SpectrogramControls::markSettingsDirty()
{
    settingsDirty = true;
    settingsSaveTimer.start(500); /* flush 500ms after last change */
}

void SpectrogramControls::flushSettings()
{
    if (!settingsDirty)
        return;
    settingsDirty = false;
    QSettings settings;
    settings.setValue("FFTSize", fftSizeSlider->value());
    settings.setValue("ZoomLevel", zoomLevelSlider->value());
    settings.setValue("PowerMin", powerMinSlider->value());
    settings.setValue("PowerMax", powerMaxSlider->value());
}

void SpectrogramControls::fftSizeChanged(int value)
{
    (void)value;
    markSettingsDirty();
    fftOrZoomChanged();
}

void SpectrogramControls::zoomLevelChanged(int value)
{
    (void)value;
    markSettingsDirty();
    fftOrZoomChanged();
}

void SpectrogramControls::powerMinChanged(int value)
{
    (void)value;
    markSettingsDirty();
}

void SpectrogramControls::powerMaxChanged(int value)
{
    (void)value;
    markSettingsDirty();
}

void SpectrogramControls::fileOpenButtonClicked()
{
    QSettings settings;
    QString fileName;
    QFileDialog fileSelect(this);
    fileSelect.setNameFilter(tr("All files (*);;"
                "Session (*.isession);;"
                "Bookmarks (*.json);;"
                "IQ WAV (*.wav);;"
                "IQ int16 (*.cs16 *.sc16 *.c16);;"
                "IQ float32 (*.cfile *.cf32 *.fc32);;"
                "IQ int8 (*.cs8 *.sc8 *.c8);;"
                "IQ uint8 (*.cu8 *.uc8)"));

    {
        QByteArray savedState = settings.value("OpenFileState").toByteArray();
        fileSelect.restoreState(savedState);

        QString lastUsedFilter = settings.value("OpenFileFilter").toString();
        if(lastUsedFilter.size())
            fileSelect.selectNameFilter(lastUsedFilter);
    }

    if(fileSelect.exec())
    {
        fileName = fileSelect.selectedFiles()[0];

        QByteArray dialogState = fileSelect.saveState();
        settings.setValue("OpenFileState", dialogState);
        settings.setValue("OpenFileFilter", fileSelect.selectedNameFilter());
    }

    if (!fileName.isEmpty()) {
        if (fileName.endsWith(".isession", Qt::CaseInsensitive))
            emit loadSessionFile(fileName);
        else if (fileName.endsWith(".json", Qt::CaseInsensitive))
            loadBookmarksFile(fileName);
        else
            emit openFile(fileName);
    }
}

void SpectrogramControls::timeSelectionChanged(float time, float offset)
{
    if (cursorsCheckBox->checkState() == Qt::Checked && time > 0) {
        if (!offsetEdit->hasFocus())
            offsetEdit->setText(QString::number(offset, 'f', 6));
        if (!periodEdit->hasFocus())
            periodEdit->setText(QString::number(time, 'f', 6));
        rateLabel->setText(QString::fromStdString(formatSIValueSigned(1 / time, "Hz")));

        int symbols = cursorSymbolsSpinBox->value();
        if (symbols > 0) {
            symbolPeriodLabel->setText(QString::fromStdString(formatSIValueSigned(time / symbols, "s")));
            if (!symbolRateEdit->hasFocus()) {
                double symRate = symbols / time;
                symbolRateEdit->setText(QString::fromStdString(
                    formatSIValueSigned(symRate, "Bd")));
            }
        }
    }
}

void SpectrogramControls::zoomIn()
{
    zoomLevelSlider->setValue(zoomLevelSlider->value() + 1);
}

void SpectrogramControls::zoomOut()
{
    zoomLevelSlider->setValue(zoomLevelSlider->value() - 1);
}

void SpectrogramControls::tunerInfoChanged(double centreHz, double bandwidthHz)
{
    if (!tunerCentreEdit->hasFocus())
        tunerCentreEdit->setText(QString::fromStdString(
            formatSIValueSigned(centreHz, "Hz")));
    if (!tunerBandwidthEdit->hasFocus())
        tunerBandwidthEdit->setText(QString::fromStdString(
            formatSIValueSigned(bandwidthHz, "Hz")));
}

void SpectrogramControls::renderTimeChanged(int ms)
{
    /* skip 0ms readings -- pure cache hits with no real work */
    if (ms <= 0)
        return;

    if (ms < renderMin) renderMin = ms;
    if (ms > renderMax) renderMax = ms;
    renderSum += ms;
    renderCount++;
    int avg = (renderCount > 0) ? (int)(renderSum / renderCount) : 0;

    renderTimeLabel->setText(
        QString("%1ms [%2/%3/%4]")
            .arg(ms).arg(renderMin).arg(avg).arg(renderMax));
}

void SpectrogramControls::resetRenderStats()
{
    renderMin = INT_MAX;
    renderMax = 0;
    renderSum = 0;
    renderCount = 0;
    renderTimeLabel->setText("- ms");
}

void SpectrogramControls::viewPositionChanged(double timeSec, double freqHz)
{
    currentTimeSec = timeSec;
    currentFreqHz = freqHz;
    if (!viewPosXEdit->hasFocus())
        viewPosXEdit->setText(QString::number(timeSec, 'f', 4));
    if (!viewPosYEdit->hasFocus())
        viewPosYEdit->setText(QString::fromStdString(
            formatSIValueSigned(freqHz, "Hz")));
}

void SpectrogramControls::addBookmark()
{
    Bookmark bm;
    bm.timeSec = currentTimeSec;
    bm.freqHz = currentFreqHz;
    bm.name = QString("@%1s %2Hz")
        .arg(bm.timeSec, 0, 'f', 3)
        .arg(bm.freqHz, 0, 'f', 0);

    bookmarks.append(bm);
    bookmarkCombo->addItem(bm.name);
    bookmarkCombo->setCurrentIndex(bookmarks.size() - 1);
    saveBookmarks();
}

void SpectrogramControls::removeBookmark()
{
    int idx = bookmarkCombo->currentIndex();
    if (idx >= 0 && idx < bookmarks.size()) {
        bookmarks.removeAt(idx);
        bookmarkCombo->removeItem(idx);
        saveBookmarks();
    }
}

void SpectrogramControls::editBookmark()
{
    int idx = bookmarkCombo->currentIndex();
    if (idx < 0 || idx >= bookmarks.size())
        return;

    auto &bm = bookmarks[idx];
    bm.timeSec = currentTimeSec;
    bm.freqHz = currentFreqHz;
    bm.name = QString("@%1s %2Hz")
        .arg(bm.timeSec, 0, 'f', 3)
        .arg(bm.freqHz, 0, 'f', 0);

    bookmarkCombo->setItemText(idx, bm.name);
    saveBookmarks();
}

void SpectrogramControls::onBookmarkActivated(int index)
{
    if (index >= 0 && index < bookmarks.size())
        emit bookmarkSelected(bookmarks[index].timeSec, bookmarks[index].freqHz);
}

void SpectrogramControls::saveBookmarks()
{
    QJsonDocument doc(getBookmarksJson());
    QString path = QCoreApplication::applicationDirPath() + "/bookmarks.json";
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(doc.toJson());
}

void SpectrogramControls::loadBookmarks()
{
    loadBookmarksFile(
        QCoreApplication::applicationDirPath() + "/bookmarks.json");
}

void SpectrogramControls::loadBookmarksFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    setBookmarksJson(doc.array());
}

QJsonArray SpectrogramControls::getBookmarksJson()
{
    QJsonArray arr;
    for (auto &bm : bookmarks) {
        QJsonObject obj;
        obj["name"] = bm.name;
        obj["timeSec"] = bm.timeSec;
        obj["freqHz"] = bm.freqHz;
        arr.append(obj);
    }
    return arr;
}

void SpectrogramControls::setBookmarksJson(const QJsonArray &arr)
{
    bookmarks.clear();
    bookmarkCombo->clear();

    for (auto val : arr) {
        QJsonObject obj = val.toObject();
        Bookmark bm;
        bm.name = obj["name"].toString();
        bm.timeSec = obj["timeSec"].toDouble();
        bm.freqHz = obj["freqHz"].toDouble();
        bookmarks.append(bm);
        bookmarkCombo->addItem(bm.name);
    }
}

void SpectrogramControls::enableAnnotations(bool enabled) {
    commentsCheckBox->setEnabled(enabled);
}
