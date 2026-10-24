#include "thumbnailloader.h"

#include "library.h"

#include <QElapsedTimer>

#include <cstdio>
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
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        for (quint64 key : keys) {
            if (!m_inFlight.contains(key) && !m_readyKeys.contains(key) && !m_failed.contains(key))
                m_queue.push_back(key);
        }
    }
    m_wake.notify_all();
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
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop)
                break;
            key = m_queue.front();
            m_queue.pop_front();
            m_inFlight.insert(key);
        }

        timer.start();
        const int index = keyIndex(key);
        QImage image;
        if (index >= 0 && index < m_library->count())
            image = decodeJpeg(tj, m_library->at(index).thumbPath, keyLevel(key), buffer);
        m_decodeNanos += quint64(timer.nsecsElapsed());
        m_decoded += 1;

        bool notify = false;
        {
            std::lock_guard lock(m_mutex);
            m_inFlight.erase(key);
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
