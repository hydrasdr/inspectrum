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

#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QTimer>
#include <QLabel>

class SpectrogramControls : public QDockWidget
{
    Q_OBJECT

public:
    SpectrogramControls(const QString & title, QWidget * parent);
    void setDefaults();

signals:
    void fftOrZoomChanged(int fftSize, int zoomLevel);
    void openFile(QString fileName);
    void loadSessionFile(QString fileName);
    void symbolRateChanged(double rate);
    void saveSession();
    void autoDetectRate();
    void lsbFirstChanged(bool lsb);
    void periodChanged(double seconds);
    void offsetChanged(double seconds);
    void tunerCentreEdited(double hz);
    void tunerBandwidthEdited(double hz);
    void bookmarkSelected(double timeSec, double freqHz);
    void avgChanged(int level);
    void tunerVisibleChanged(bool visible);
    void viewPosXEdited(double timeSec);
    void viewPosYEdited(double freqHz);

public slots:
    void timeSelectionChanged(float time, float offset);
    void zoomIn();
    void zoomOut();
    void tunerInfoChanged(double centreHz, double bandwidthHz);
    void renderTimeChanged(int ms);
    void resetRenderStats();
    void viewPositionChanged(double timeSec, double freqHz);
    void addBookmark();
    void removeBookmark();
    void editBookmark();
    void onBookmarkActivated(int index);
    void enableAnnotations(bool enabled);

private slots:
    void fftSizeChanged(int value);
    void zoomLevelChanged(int value);
    void powerMinChanged(int value);
    void powerMaxChanged(int value);
    void fileOpenButtonClicked();
    void cursorsStateChanged(int state);

private:
    QWidget *widget;
    QFormLayout *layout;
    void clearCursorLabels();
    void fftOrZoomChanged(void);

public:
    QLineEdit *viewPosXEdit;
    QLineEdit *viewPosYEdit;
    QComboBox *bookmarkCombo;
    QPushButton *fileOpenButton;
    QPushButton *saveSessionButton;
    QLineEdit *sampleRate;
    QSlider *fftSizeSlider;
    QLabel *fftSizeLabel;
    QSlider *zoomLevelSlider;
    QLabel *zoomLabel;
    QSlider *zeroPadSlider;
    QLabel *zeroPadLabel;
    QSlider *zoomYSlider;
    QLabel *zoomYLabel;
    QSlider *powerMaxSlider;
    QLabel *powerMaxLabel;
    QSlider *powerMinSlider;
    QLabel *powerMinLabel;
    QCheckBox *cursorsCheckBox;
    QSlider *cursorGridSlider;
    QLabel *gridOpacityLabel;
    QSpinBox *cursorSymbolsSpinBox;
    QLineEdit *offsetEdit;
    QLabel *rateLabel;
    QLineEdit *periodEdit;
    QLineEdit *symbolRateEdit;
    QLabel *symbolPeriodLabel;
    QPushButton *autoDetectButton;
    QLabel *detectStatusLabel;
    QCheckBox *lsbFirstCheckBox;
    QLineEdit *tunerCentreEdit;
    QLineEdit *tunerBandwidthEdit;
    QCheckBox *maskOutOfBandCheckBox;
    QCheckBox *tunerVisibleCheckBox;
    QLabel *renderTimeLabel;
    QPushButton *renderResetButton;
    struct Bookmark {
        QString name;
        double timeSec;
        double freqHz;
    };
    QList<Bookmark> bookmarks;
    double currentTimeSec = 0;
    double currentFreqHz = 0;
    void saveBookmarks();
    void loadBookmarks();
    void loadBookmarksFile(const QString &path);
    QJsonArray getBookmarksJson();
    void setBookmarksJson(const QJsonArray &arr);

    int renderMin = INT_MAX, renderMax = 0;
    double renderSum = 0;
    int renderCount = 0;

    /* debounced QSettings persistence (avoids I/O on every slider tick) */
    QTimer settingsSaveTimer;
    bool settingsDirty = false;
    void markSettingsDirty();
    void flushSettings();
    QCheckBox *scalesCheckBox;
    QCheckBox *annosCheckBox;
    QCheckBox *commentsCheckBox;

    QSlider *avgSlider;
    QLabel *avgLabel;
};
