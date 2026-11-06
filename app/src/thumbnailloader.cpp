#include "thumbnailloader.h"

#include "colour.h"
#include "library.h"
#include "photoprovider.h"

#include <QElapsedTimer>
#include <QFile>
#include <cmath>
#include <QtEndian>

#include <cstdio>
#include <cstring>
#include <turbojpeg.h>

namespace {

// Reads and decodes one JPEG at 1/(2^level) of its size into RGBX, the
// layout the GPU takes without conversion, in sRGB: its ICC profile, read
// with it, says what it was in (colour.h). Given `icc`, the profile is
// handed back there instead, to convert later (a mosaic's tile, once
// small). A null image means the file is missing or broken; the grid keeps
// showing the cell's average colour.
QImage decodeJpeg(tjhandle tj, const QByteArray &path, int level, std::vector<unsigned char> &buffer, QByteArray *icc = nullptr) {
    FILE *f = std::fopen(path.constData(), "rb");
    if (!f)
        return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return {};
    }
    buffer.resize(size_t(size));
    const size_t got = std::fread(buffer.data(), 1, buffer.size(), f);
    std::fclose(f);
    if (got != buffer.size())
        return {};

    if (tj3DecompressHeader(tj, buffer.data(), buffer.size()) != 0)
        return {};
    const int width = tj3Get(tj, TJPARAM_JPEGWIDTH);
    const int height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    const tjscalingfactor factor{1, 1 << level};
    if (tj3SetScalingFactor(tj, factor) != 0)
        return {};
    QImage image(TJSCALED(width, factor), TJSCALED(height, factor), QImage::Format_RGBX8888);
    if (tj3Decompress8(tj, buffer.data(), buffer.size(), image.bits(), int(image.bytesPerLine()), TJPF_RGBX) != 0)
        return {};
    QByteArray profile;
    unsigned char *data = nullptr;
    size_t length = 0;
    if (tj3GetICCProfile(tj, &data, &length) == 0 && data) {
        profile = QByteArray(reinterpret_cast<const char *>(data), qsizetype(length));
        tj3Free(data);
    }
    if (icc)
        *icc = profile;
    else
        toSrgb(image, profile);
    return image;
}

// One mosaic page: each photo decoded at the smallest size that still
// covers a tile, its centred square scaled to the tile. A photo that will
// not decode keeps its average colour.
QImage buildPage(tjhandle tj, const Library &library, int page, std::vector<unsigned char> &buffer) {
    const int side = kMosaicSide * kMosaicTile;
    QImage out(side, side, QImage::Format_RGBX8888);
    out.fill(QColor(16, 16, 16));
    for (int j = 0; j < kMosaicCount; ++j) {
        const int index = page * kMosaicCount + j;
        if (index >= library.count())
            break;
        const LibraryItem &item = library.at(index);
        const int x = (j % kMosaicSide) * kMosaicTile;
        const int y = (j / kMosaicSide) * kMosaicTile;
        int level = 0;
        const int shortEdge = std::min(item.thumbW, item.thumbH);
        while (level < 3 && (shortEdge >> (level + 1)) >= kMosaicTile)
            level += 1;
        QByteArray icc;
        const QImage image = decodeJpeg(tj, item.thumbPath, level, buffer, &icc);
        if (image.isNull()) {
            const QRgb c = item.avgColor;
            for (int row = 0; row < kMosaicTile; ++row) {
                auto *line = reinterpret_cast<quint32 *>(out.scanLine(y + row)) + x;
                std::fill(line, line + kMosaicTile, qToLittleEndian(0xff000000u | (qBlue(c) << 16) | (qGreen(c) << 8) | qRed(c)));
            }
            continue;
        }
        const int s = std::min(image.width(), image.height());
        QImage tile = image.copy((image.width() - s) / 2, (image.height() - s) / 2, s, s)
                          .scaled(kMosaicTile, kMosaicTile, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_RGBX8888);
        toSrgb(tile, icc);
        for (int row = 0; row < kMosaicTile; ++row)
            std::memcpy(out.scanLine(y + row) + x * 4, tile.constScanLine(row), kMosaicTile * 4);
    }
    return out;
}

} // namespace

QImage decodeThumbnail(const QByteArray &path, int level) {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    tj3Set(tj, TJPARAM_SAVEMARKERS, 4);
    std::vector<unsigned char> buffer;
    QImage image = decodeJpeg(tj, path, level, buffer);
    tj3Destroy(tj);
    return image;
}

ThumbnailLoader::ThumbnailLoader(const Library *library, int threads, QObject *parent)
    : QObject(parent), m_library(library) {
    for (int i = 0; i < threads; ++i)
        m_threads.emplace_back([this, i] { run(i < 2); });
}

ThumbnailLoader::~ThumbnailLoader() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (auto &t : m_threads)
        t.join();
}

void ThumbnailLoader::setWanted(const std::vector<quint64> &keys) {
    QList<int> ask, askFull;
    quint64 generation;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        m_queue.clear();
        for (quint64 key : keys) {
            if (isFullKey(key)) {
                const int index = keyIndex(key);
                if (m_fullMissing.contains(index)) continue;
                if (!m_fullPaths.contains(index)) {
                    if (m_fullAsked.insert(index).second) askFull.append(index);
                    continue;
                }
            }
            if (isMediumKey(key)) {
                // A medium rendition decodes once its file is here; until
                // then pinholed is asked for it, once.
                const int index = keyIndex(key);
                if (m_mediumMissing.contains(index))
                    continue;
                if (!m_mediumPaths.contains(index)) {
                    if (m_mediumAsked.insert(index).second)
                        ask.append(index);
                    continue;
                }
            }
            if (!m_inFlight.contains(key) && !m_readyKeys.contains(key) && !m_failed.contains(key))
                m_queue.push_back(key);
        }
    }
    m_wake.notify_all();
    if (!ask.isEmpty())
        emit mediumNeeded(ask, generation);
    if (!askFull.isEmpty()) emit fullNeeded(askFull, generation);
}

void ThumbnailLoader::setMediumPath(int index, const QByteArray &path, quint64 generation) {
    std::lock_guard lock(m_mutex);
    if (generation == m_generation)
        m_mediumPaths[index] = path;
}

void ThumbnailLoader::setMediumUnavailable(int index, quint64 generation) {
    std::lock_guard lock(m_mutex);
    if (generation == m_generation)
        m_mediumMissing.insert(index);
}

void ThumbnailLoader::clearMedium(int index, quint64 generation) {
    std::lock_guard lock(m_mutex);
    if (generation != m_generation)
        return;
    m_mediumMissing.erase(index);
    m_mediumAsked.erase(index);
}

bool ThumbnailLoader::mediumUnavailable(int index) {
    std::lock_guard lock(m_mutex);
    return m_mediumMissing.contains(index);
}

void ThumbnailLoader::setFullPath(int index, const QByteArray &path, quint64 generation) {
    std::lock_guard lock(m_mutex);
    if (generation == m_generation) m_fullPaths[index] = path;
}

void ThumbnailLoader::setFullUnavailable(int index, quint64 generation) {
    std::lock_guard lock(m_mutex);
    if (generation == m_generation) m_fullMissing.insert(index);
}

void ThumbnailLoader::clearFull(int index, quint64 generation) {
    std::lock_guard lock(m_mutex);
    if (generation != m_generation) return;
    m_fullMissing.erase(index);
    m_fullAsked.erase(index);
}

std::vector<DecodedThumb> ThumbnailLoader::takeReady(size_t max) {
    std::lock_guard lock(m_mutex);
    std::vector<DecodedThumb> out;
    if (m_ready.size() <= max) {
        out.swap(m_ready);
    } else {
        // Oldest results first: they were asked for first.
        out.assign(std::make_move_iterator(m_ready.begin()), std::make_move_iterator(m_ready.begin() + long(max)));
        m_ready.erase(m_ready.begin(), m_ready.begin() + long(max));
    }
    for (const auto &d : out)
        m_readyKeys.erase(d.key);
    m_signalPending = !m_ready.empty();
    return out;
}

void ThumbnailLoader::reload(const std::function<void()> &swap) {
    std::unique_lock lock(m_mutex);
    m_paused = true;
    m_queue.clear();
    m_giveUp = true;
    m_idle.wait(lock, [this] { return m_inFlight.empty(); });
    m_giveUp = false;
    swap();
    m_ready.clear();
    m_readyKeys.clear();
    m_failed.clear();
    m_mediumPaths.clear();
    m_mediumAsked.clear();
    m_mediumMissing.clear();
    m_fullPaths.clear();
    m_fullAsked.clear();
    m_fullMissing.clear();
    ++m_generation;
    m_signalPending = false;
    m_paused = false;
}

size_t ThumbnailLoader::queued() {
    std::lock_guard lock(m_mutex);
    return m_queue.size();
}

void ThumbnailLoader::run(bool fullDecoder) {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    // The ICC profile (APP2) read with the header, and no other marker.
    tj3Set(tj, TJPARAM_SAVEMARKERS, 4);
    std::vector<unsigned char> buffer;
    QElapsedTimer timer;
    for (;;) {
        quint64 key;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this, fullDecoder] { return m_stop || (!m_paused && !m_queue.empty()
                && (!isFullKey(m_queue.front()) || (fullDecoder && m_fullActive < 2))); });
            if (m_stop)
                break;
            key = m_queue.front();
            m_queue.pop_front();
            m_inFlight.insert(key);
            if (isFullKey(key)) ++m_fullActive;
        }

        timer.start();
        QImage image;
        if (isPageKey(key)) {
            image = buildPage(tj, *m_library, keyPage(key), buffer);
        } else if (isFullKey(key)) {
            QByteArray path;
            {
                std::lock_guard lock(m_mutex);
                path = m_fullPaths[keyIndex(key)];
            }
            const int index = keyIndex(key);
            if (index >= 0 && index < m_library->count()) {
                const auto &item = m_library->at(index);
                const double shortEdge = std::max(1, int(std::min(item.thumbW, item.thumbH)));
                const double scale = fullShortEdge(keyLevel(key)) / shortEdge;
                // Cover the square crop, not a bounding square: a tall
                // screenshot needs its short edge sharp, too.
                const QSize bounds = item.thumbW && item.thumbH
                    ? QSize(std::max(1, int(std::ceil(item.thumbW * scale))), std::max(1, int(std::ceil(item.thumbH * scale))))
                    : QSize(fullShortEdge(keyLevel(key)), fullShortEdge(keyLevel(key)));
                image = decodePhoto(QFile::decodeName(path), bounds, &m_giveUp);
                image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBX8888);
            }
        } else if (isMediumKey(key)) {
            QByteArray path;
            {
                std::lock_guard lock(m_mutex);
                path = m_mediumPaths[keyIndex(key)];
            }
            image = decodeJpeg(tj, path, keyLevel(key), buffer);
            if (image.isNull()) {
                image = decodePhoto(QFile::decodeName(path), QSize(), &m_giveUp);
                if (!image.isNull() && keyLevel(key) > 0)
                    image = image.scaled(std::max(1, image.width() >> keyLevel(key)), std::max(1, image.height() >> keyLevel(key)),
                                         Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
        } else if (const int index = keyIndex(key); index >= 0 && index < m_library->count()) {
            image = decodeJpeg(tj, m_library->at(index).thumbPath, keyLevel(key), buffer);
        }
        const quint64 took = quint64(timer.nsecsElapsed());
        m_decodeNanos += took;
        m_decoded += 1;
        const int kind = isPageKey(key) ? 2 : isDetailKey(key) ? 1 : 0;
        m_kindNanos[kind] += took;
        m_kindCount[kind] += 1;

        bool notify = false;
        {
            std::lock_guard lock(m_mutex);
            m_inFlight.erase(key);
            if (isFullKey(key)) --m_fullActive;
            if (m_inFlight.empty())
                m_idle.notify_all();
            if (image.isNull()) {
                m_failed.insert(key);
                if (isMediumKey(key)) {
                    m_mediumMissing.insert(keyIndex(key));
                    notify = !m_signalPending.exchange(true);
                }
            } else {
                m_ready.push_back({key, std::move(image)});
                m_readyKeys.insert(key);
                notify = !m_signalPending.exchange(true);
            }
        }
        if (isFullKey(key)) m_wake.notify_all();
        if (notify)
            emit ready();
    }
    tj3Destroy(tj);
}
