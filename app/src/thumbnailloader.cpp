#include "thumbnailloader.h"

#include "library.h"

#include <QElapsedTimer>
#include <QtEndian>

#include <cstdio>
#include <cstring>
#include <turbojpeg.h>

namespace {

// Reads and decodes one JPEG at 1/(2^level) of its size into RGBX, the
// layout the GPU takes without conversion. A null image means the file is
// missing or broken; the grid keeps showing the cell's average colour.
QImage decodeJpeg(tjhandle tj, const QByteArray &path, int level, std::vector<unsigned char> &buffer) {
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
        const QImage image = decodeJpeg(tj, item.thumbPath, level, buffer);
        if (image.isNull()) {
            const QRgb c = item.avgColor;
            for (int row = 0; row < kMosaicTile; ++row) {
                auto *line = reinterpret_cast<quint32 *>(out.scanLine(y + row)) + x;
                std::fill(line, line + kMosaicTile, qToLittleEndian(0xff000000u | (qBlue(c) << 16) | (qGreen(c) << 8) | qRed(c)));
            }
            continue;
        }
        const int s = std::min(image.width(), image.height());
        const QImage tile = image.copy((image.width() - s) / 2, (image.height() - s) / 2, s, s)
                                .scaled(kMosaicTile, kMosaicTile, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                .convertToFormat(QImage::Format_RGBX8888);
        for (int row = 0; row < kMosaicTile; ++row)
            std::memcpy(out.scanLine(y + row) + x * 4, tile.constScanLine(row), kMosaicTile * 4);
    }
    return out;
}

} // namespace

ThumbnailLoader::ThumbnailLoader(const Library *library, int threads, QObject *parent)
    : QObject(parent), m_library(library) {
    for (int i = 0; i < threads; ++i)
        m_threads.emplace_back([this] { run(); });
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
    QList<int> ask;
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        for (quint64 key : keys) {
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
        emit mediumNeeded(ask);
}

void ThumbnailLoader::setMediumPath(int index, const QByteArray &path) {
    std::lock_guard lock(m_mutex);
    m_mediumPaths[index] = path;
}

void ThumbnailLoader::setMediumUnavailable(int index) {
    std::lock_guard lock(m_mutex);
    m_mediumMissing.insert(index);
}

void ThumbnailLoader::clearMedium(int index) {
    std::lock_guard lock(m_mutex);
    m_mediumMissing.erase(index);
    m_mediumAsked.erase(index);
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
    m_idle.wait(lock, [this] { return m_inFlight.empty(); });
    swap();
    m_ready.clear();
    m_readyKeys.clear();
    m_failed.clear();
    m_mediumPaths.clear();
    m_mediumAsked.clear();
    m_mediumMissing.clear();
    m_signalPending = false;
    m_paused = false;
}

size_t ThumbnailLoader::queued() {
    std::lock_guard lock(m_mutex);
    return m_queue.size();
}

void ThumbnailLoader::run() {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    std::vector<unsigned char> buffer;
    QElapsedTimer timer;
    for (;;) {
        quint64 key;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || (!m_paused && !m_queue.empty()); });
            if (m_stop)
                break;
            key = m_queue.front();
            m_queue.pop_front();
            m_inFlight.insert(key);
        }

        timer.start();
        QImage image;
        if (isPageKey(key)) {
            image = buildPage(tj, *m_library, keyPage(key), buffer);
        } else if (isMediumKey(key)) {
            QByteArray path;
            {
                std::lock_guard lock(m_mutex);
                path = m_mediumPaths[keyIndex(key)];
            }
            image = decodeJpeg(tj, path, keyLevel(key), buffer);
        } else if (const int index = keyIndex(key); index >= 0 && index < m_library->count()) {
            image = decodeJpeg(tj, m_library->at(index).thumbPath, keyLevel(key), buffer);
        }
        m_decodeNanos += quint64(timer.nsecsElapsed());
        m_decoded += 1;

        bool notify = false;
        {
            std::lock_guard lock(m_mutex);
            m_inFlight.erase(key);
            if (m_inFlight.empty())
                m_idle.notify_all();
            if (image.isNull()) {
                m_failed.insert(key);
            } else {
                m_ready.push_back({key, std::move(image)});
                m_readyKeys.insert(key);
                notify = !m_signalPending.exchange(true);
            }
        }
        if (notify)
            emit ready();
    }
    tj3Destroy(tj);
}
