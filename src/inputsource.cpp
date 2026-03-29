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

#include "inputsource.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <stdexcept>
#include <algorithm>

#include <QFileInfo>

#include <QElapsedTimer>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmapCache>
#include <QRect>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>


class ComplexF32SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<float>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<float>*>(src);
        std::copy(&s[start], &s[start + length], dest);
    }
};

class ComplexF64SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<double>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<double>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<double>& v) -> std::complex<float> {
                return { static_cast<float>(v.real()) , static_cast<float>(v.imag()) };
            }
        );
    }
};

class ComplexS32SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<int32_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<int32_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<int32_t>& v) -> std::complex<float> {
                const float k = 1.0f / 2147483648.0f;
                return { v.real() * k, v.imag() * k };
            }
        );
    }
};

class ComplexS16SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<int16_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<int16_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<int16_t>& v) -> std::complex<float> {
                const float k = 1.0f / 32768.0f;
                return { v.real() * k, v.imag() * k };
            }
        );
    }
};

class ComplexS8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<int8_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<int8_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<int8_t>& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { v.real() * k, v.imag() * k };
            }
        );
    }
};

class ComplexU8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(std::complex<uint8_t>);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const std::complex<uint8_t>*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const std::complex<uint8_t>& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { (v.real() - 127.4f) * k, (v.imag() - 127.4f) * k };
            }
        );
    }
};

class RealF32SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(float);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const float*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const float& v) -> std::complex<float> {
                return {v, 0.0f};
            }
        );
    }
};

class RealF64SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(double);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const double*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const double& v) -> std::complex<float> {
                return {static_cast<float>(v), 0.0f};
            }
        );
    }
};

class RealS16SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(int16_t);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const int16_t*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const int16_t& v) -> std::complex<float> {
                const float k = 1.0f / 32768.0f;
                return { v * k, 0.0f };
            }
        );
    }
};

class RealS8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(int8_t);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const int8_t*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const int8_t& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { v * k, 0.0f };
            }
        );
    }
};

class RealU8SampleAdapter : public SampleAdapter {
public:
    size_t sampleSize() override {
        return sizeof(uint8_t);
    }

    void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) override {
        auto s = reinterpret_cast<const uint8_t*>(src);
        std::transform(&s[start], &s[start + length], dest,
            [](const uint8_t& v) -> std::complex<float> {
                const float k = 1.0f / 128.0f;
                return { (v - 127.4f) * k, 0 };
            }
        );
    }
};

InputSource::InputSource()
{
}

InputSource::~InputSource()
{
    cleanup();
}

void InputSource::cleanup()
{
    if (mmapBase != nullptr && inputFile != nullptr) {
        inputFile->unmap(mmapBase);
        mmapBase = nullptr;
        mmapData = nullptr;
    }

    if (inputFile != nullptr) {
        delete inputFile;
        inputFile = nullptr;
    }
}

QJsonObject InputSource::readMetaData(const QString &filename)
{
    QFile datafile(filename);
    if (!datafile.open(QFile::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("Error while opening meta data file: " + datafile.errorString().toStdString());
    }

    QJsonDocument d = QJsonDocument::fromJson(datafile.readAll());
    datafile.close();
    auto root = d.object();

    if (!root.contains("global") || !root["global"].isObject()) {
        throw std::runtime_error("SigMF meta data is invalid (no global object found)");
    }

    auto global = root["global"].toObject();

    if (!global.contains("core:datatype") || !global["core:datatype"].isString()) {
        throw std::runtime_error("SigMF meta data does not specify a valid datatype");
    }


    auto datatype = global["core:datatype"].toString();
    if (datatype.compare("cf32_le") == 0) {
        sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
    } else if (datatype.compare("ci32_le") == 0) {
        sampleAdapter = std::make_unique<ComplexS32SampleAdapter>();
    } else if (datatype.compare("ci16_le") == 0) {
        sampleAdapter = std::make_unique<ComplexS16SampleAdapter>();
    } else if (datatype.compare("ci8") == 0) {
        sampleAdapter = std::make_unique<ComplexS8SampleAdapter>();
    } else if (datatype.compare("cu8") == 0) {
        sampleAdapter = std::make_unique<ComplexU8SampleAdapter>();
    } else if (datatype.compare("rf32_le") == 0) {
        sampleAdapter = std::make_unique<RealF32SampleAdapter>();
        _realSignal = true;
    } else if (datatype.compare("ri16_le") == 0) {
        sampleAdapter = std::make_unique<RealS16SampleAdapter>();
        _realSignal = true;
    } else if (datatype.compare("ri8") == 0) {
        sampleAdapter = std::make_unique<RealS8SampleAdapter>();
        _realSignal = true;
    } else if (datatype.compare("ru8") == 0) {
        sampleAdapter = std::make_unique<RealU8SampleAdapter>();
        _realSignal = true;
    } else {
        throw std::runtime_error("SigMF meta data specifies unsupported datatype");
    }

    if (global.contains("core:sample_rate") && global["core:sample_rate"].isDouble()) {
        setSampleRate(global["core:sample_rate"].toDouble());
    }


    if (root.contains("captures") && root["captures"].isArray()) {
        auto captures = root["captures"].toArray();

        for (auto capture_ref : captures) {
            if (capture_ref.isObject()) {
                auto capture = capture_ref.toObject();
                if (capture.contains("core:frequency") && capture["core:frequency"].isDouble()) {
                    frequency = capture["core:frequency"].toDouble();
                }
            } else {
                throw std::runtime_error("SigMF meta data is invalid (invalid capture object)");
            }
        }
    }

    if(root.contains("annotations") && root["annotations"].isArray()) {

        size_t offset = 0;

        if (global.contains("core:offset")) {
            offset = global["offset"].toDouble();
        }

        auto annotations = root["annotations"].toArray();

        for (auto annotation_ref : annotations) {
            if (annotation_ref.isObject()) {
                auto sigmf_annotation = annotation_ref.toObject();

                const size_t sample_start = sigmf_annotation["core:sample_start"].toDouble();

                if (sample_start < offset)
                    continue;

                const size_t rel_sample_start = sample_start - offset;

                const size_t sample_count = sigmf_annotation["core:sample_count"].toDouble();
                auto sampleRange = range_t<size_t>{rel_sample_start, rel_sample_start + sample_count - 1};

                const double freq_lower_edge = sigmf_annotation["core:freq_lower_edge"].toDouble();
                const double freq_upper_edge = sigmf_annotation["core:freq_upper_edge"].toDouble();
                auto frequencyRange = range_t<double>{freq_lower_edge, freq_upper_edge};

                auto label = sigmf_annotation["core:label"].toString();
                if (label.isEmpty()) {
                    label = sigmf_annotation["core:description"].toString();
                }

                auto comment = sigmf_annotation["core:comment"].toString();

                annotationList.emplace_back(sampleRange, frequencyRange, label, comment);
            }
        }
    }

    return root;
}

/*
 * Parse a RIFF/WAV header from memory-mapped data.
 * Supports IQ WAV files as produced by SDR++ and similar tools:
 *   - PCM  (codec 1): uint8, int16, int32  (2-channel IQ)
 *   - IEEE float (codec 3): float32         (2-channel IQ)
 * Returns the byte offset where sample data begins.
 * Sets sampleAdapter and sampleRate from the header.
 */
size_t InputSource::parseWavHeader(const uchar *data, size_t fileSize)
{
    if (fileSize < 44)
        throw std::runtime_error("WAV file too small for header");

    /* RIFF header */
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        throw std::runtime_error("Not a valid WAV file");

    /* Walk chunks to find "fmt " and "data" */
    size_t pos = 12;
    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t wavSampleRate = 0;
    uint16_t bitsPerSample = 0;
    size_t dataChunkOffset = 0;
    size_t dataChunkSize = 0;
    bool foundFmt = false;
    bool foundData = false;

    while (pos + 8 <= fileSize) {
        uint32_t chunkSize;
        memcpy(&chunkSize, data + pos + 4, 4);

        if (memcmp(data + pos, "fmt ", 4) == 0) {
            if (chunkSize < 16 || pos + 8 + chunkSize > fileSize)
                throw std::runtime_error("WAV fmt chunk too small");
            memcpy(&audioFormat, data + pos + 8, 2);
            memcpy(&numChannels, data + pos + 10, 2);
            memcpy(&wavSampleRate, data + pos + 12, 4);
            memcpy(&bitsPerSample, data + pos + 22, 2);
            foundFmt = true;
        }
        else if (memcmp(data + pos, "data", 4) == 0) {
            dataChunkOffset = pos + 8;
            dataChunkSize = chunkSize;
            foundData = true;
            break;
        }

        pos += 8 + chunkSize;
        /* Chunks are word-aligned */
        if (chunkSize & 1) pos++;
    }

    if (!foundFmt)
        throw std::runtime_error("WAV file missing fmt chunk");
    if (!foundData)
        throw std::runtime_error("WAV file missing data chunk");

    if (numChannels != 2)
        throw std::runtime_error("IQ WAV file must have 2 channels (I/Q), got "
                                 + std::to_string(numChannels));

    /* Select adapter based on codec + bit depth */
    if (audioFormat == 1) { /* PCM */
        switch (bitsPerSample) {
        case 8:
            sampleAdapter = std::make_unique<ComplexU8SampleAdapter>();
            break;
        case 16:
            sampleAdapter = std::make_unique<ComplexS16SampleAdapter>();
            break;
        case 32:
            sampleAdapter = std::make_unique<ComplexS32SampleAdapter>();
            break;
        default:
            throw std::runtime_error("WAV: unsupported PCM bit depth "
                                     + std::to_string(bitsPerSample));
        }
    }
    else if (audioFormat == 3) { /* IEEE float */
        if (bitsPerSample == 32)
            sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
        else
            throw std::runtime_error("WAV: unsupported float bit depth "
                                     + std::to_string(bitsPerSample));
    }
    else {
        throw std::runtime_error("WAV: unsupported audio format "
                                 + std::to_string(audioFormat));
    }

    setSampleRate(static_cast<double>(wavSampleRate));

    /* Clamp data size to actual file */
    if (dataChunkOffset + dataChunkSize > fileSize)
        dataChunkSize = fileSize - dataChunkOffset;

    sampleCount = dataChunkSize / sampleAdapter->sampleSize();

    return dataChunkOffset;
}

void InputSource::openFile(const char *filename)
{
    fileName = QString::fromUtf8(filename);
    QFileInfo fileInfo(filename);
    std::string suffix = std::string(fileInfo.suffix().toLower().toUtf8().constData());
    if (_fmt != "") { suffix = _fmt; } // allow fmt override
    dataOffset = 0;
    _realSignal = false;

    if (suffix == "wav") {
        /* WAV files are parsed after mmap; adapter set by parseWavHeader */
    }
    else if ((suffix == "cfile") || (suffix == "cf32")  || (suffix == "fc32")) {
        sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
    }
    else if ((suffix == "cf64")  || (suffix == "fc64")) {
        sampleAdapter = std::make_unique<ComplexF64SampleAdapter>();
    }
    else if ((suffix == "cs32") || (suffix == "sc32") || (suffix == "c32")) {
        sampleAdapter = std::make_unique<ComplexS32SampleAdapter>();
    }
    else if ((suffix == "cs16") || (suffix == "sc16") || (suffix == "c16")) {
        sampleAdapter = std::make_unique<ComplexS16SampleAdapter>();
    }
    else if ((suffix == "cs8") || (suffix == "sc8") || (suffix == "c8")) {
        sampleAdapter = std::make_unique<ComplexS8SampleAdapter>();
    }
    else if ((suffix == "cu8") || (suffix == "uc8")) {
        sampleAdapter = std::make_unique<ComplexU8SampleAdapter>();
    }
    else if (suffix == "f32") {
        sampleAdapter = std::make_unique<RealF32SampleAdapter>();
        _realSignal = true;
    }
    else if (suffix == "f64") {
        sampleAdapter = std::make_unique<RealF64SampleAdapter>();
        _realSignal = true;
    }
    else if (suffix == "s16") {
        sampleAdapter = std::make_unique<RealS16SampleAdapter>();
        _realSignal = true;
    }
    else if (suffix == "s8") {
        sampleAdapter = std::make_unique<RealS8SampleAdapter>();
        _realSignal = true;
    }
    else if (suffix == "u8") {
        sampleAdapter = std::make_unique<RealU8SampleAdapter>();
        _realSignal = true;
    }
    else {
        sampleAdapter = std::make_unique<ComplexF32SampleAdapter>();
    }

    QString dataFilename;

    annotationList.clear();
    QString metaFilename;

    if (suffix == "sigmf-meta" || suffix == "sigmf-data" || suffix == "sigmf-") {
        dataFilename = fileInfo.path() + "/" + fileInfo.completeBaseName() + ".sigmf-data";
        metaFilename = fileInfo.path() + "/" + fileInfo.completeBaseName() + ".sigmf-meta";
        auto metaData = readMetaData(metaFilename);
        QFile datafile(dataFilename);
        if (!datafile.open(QFile::ReadOnly | QIODevice::Text)) {
            auto global = metaData["global"].toObject();
            if (global.contains("core:dataset")) {
                auto datasetfilename = global["core:dataset"].toString();
                if(QFileInfo(datasetfilename).isAbsolute()){
                    dataFilename = datasetfilename;
                }
                else{
                    dataFilename = fileInfo.path() + "/" + datasetfilename;
                }
            }
        }
    }
    else if (suffix == "sigmf") {
        throw std::runtime_error("SigMF archives are not supported. Consider extracting a recording.");
    }
    else {
        dataFilename = filename;
    }

    auto file = std::make_unique<QFile>(dataFilename);
    if (!file->open(QFile::ReadOnly)) {
        throw std::runtime_error(file->errorString().toStdString());
    }

    auto size = file->size();

    auto data = file->map(0, size);
    if (data == nullptr)
        throw std::runtime_error("Error mmapping file");

    if (suffix == "wav") {
        dataOffset = parseWavHeader(data, size);
    } else {
        sampleCount = size / sampleAdapter->sampleSize();
    }

    cleanup();

    inputFile = file.release();
    mmapBase = data;
    mmapData = data + dataOffset;

    invalidate();
}

void InputSource::setSampleRate(double rate)
{
    sampleRate = rate;
    invalidate();
}

double InputSource::rate()
{
    return sampleRate;
}

std::unique_ptr<std::complex<float>[]> InputSource::getSamples(size_t start, size_t length)
{
    if (inputFile == nullptr)
        return nullptr;

    if (mmapData == nullptr)
        return nullptr;

    if (length == 0 || start >= sampleCount)
        return nullptr;

    auto dest = std::make_unique<std::complex<float>[]>(length);

    /* handle partial reads at end of file: copy available
     * samples, zero-fill the rest */
    size_t available = std::min(length, sampleCount - start);
    sampleAdapter->copyRange(mmapData, start, available, dest.get());

    for (size_t i = available; i < length; i++)
        dest[i] = {0, 0};

    return dest;
}

bool InputSource::getSamples(size_t start, size_t length, std::complex<float>* dest)
{
    if (inputFile == nullptr || mmapData == nullptr)
        return false;

    if (length == 0 || start >= sampleCount)
        return false;

    size_t available = std::min(length, sampleCount - start);
    sampleAdapter->copyRange(mmapData, start, available, dest);

    if (available < length)
        memset(&dest[available], 0, (length - available) * sizeof(std::complex<float>));

    return true;
}

void InputSource::setFormat(std::string fmt){
    _fmt = fmt;
}
