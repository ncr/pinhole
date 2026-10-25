#pragma once

#include <QImage>
#include <QObject>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Library;

// A thumbnail at one of four decode levels: 0 is the stored size, then 1/2,
// 1/4 and 1/8, which libjpeg-turbo produces while decoding at a fraction of
// the cost.
inline quint64 thumbKey(int index, int level) { return (quint64(index) << 2) | quint64(level); }
inline int keyIndex(quint64 key) { return int((key & ~(quint64(3) << 62)) >> 2); }
inline int keyLevel(quint64 key) { return int(key & 3); }

// The medium rendition (2048 px, from iCloud through pinholed) at a decode
// level, for cells too big for the 480 px thumbnail. Marked by bit 62.
inline quint64 mediumKey(int index, int level) { return (quint64(1) << 62) | thumbKey(index, level); }
inline bool isMediumKey(quint64 key) { return (key >> 62) == 1; }

// A mosaic page: 256 consecutive photos as 64 px squares, 16 by 16, in one
// 1024 px image. When cells are this small, one page replaces 256 decodes
// and 256 texture uploads, and on Asahi it is the count of uploads, not
// their size, that stalls the GPU. Pages share the key space with
// thumbnails, marked by the top bit.
constexpr int kMosaicTile = 64;
constexpr int kMosaicSide = 16;
constexpr int kMosaicCount = kMosaicSide * kMosaicSide;
inline quint64 pageKey(int page) { return (quint64(1) << 63) | quint64(page); }
inline bool isPageKey(quint64 key) { return key >> 63; }
inline int keyPage(quint64 key) { return int(key & ~(quint64(1) << 63)); }

struct DecodedThumb {
    quint64 key;
    QImage image;
};

// Decodes thumbnails on worker threads. The grid tells it every frame what
// it wants, most wanted first; the list replaces the previous one, so
// anything that scrolled away before a worker got to it is simply dropped.
class ThumbnailLoader : public QObject {
    Q_OBJECT
public:
    ThumbnailLoader(const Library *library, int threads, QObject *parent = nullptr);
    ~ThumbnailLoader() override;

    void setWanted(const std::vector<quint64> &keys);
    // Everything decoded since the last call, up to `max`. Any thread.
    std::vector<DecodedThumb> takeReady(size_t max);

    // Where a photo's medium rendition is on disk, once pinholed has it, or
    // that it has none. Main thread.
    void setMediumPath(int index, const QByteArray &path);
    void setMediumUnavailable(int index);
    // Forget about a photo's medium rendition, so it is asked for again.
    void clearMedium(int index);

    // Run `swap` with every decoder idle, then forget everything keyed by
    // position (queued, decoded, failed, medium files): the positions mean
    // other photos now. GUI thread.
    void reload(const std::function<void()> &swap);

    int threads() const { return int(m_threads.size()); }
    quint64 decodedCount() const { return m_decoded.load(); }
    quint64 decodeNanos() const { return m_decodeNanos.load(); }
    size_t queued();

signals:
    // Emitted from a worker thread, at most once until takeReady().
    void ready();
    // Medium renditions wanted whose files are not known yet: ask pinholed.
    // Emitted from the render thread.
    void mediumNeeded(const QList<int> &indexes);

private:
    void run();

    const Library *m_library;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    bool m_paused = false;
    std::deque<quint64> m_queue;
    std::unordered_set<quint64> m_inFlight;
    std::unordered_set<quint64> m_readyKeys;
    // Files that would not decode; asked for again they would be retried
    // every frame.
    std::unordered_set<quint64> m_failed;
    std::vector<DecodedThumb> m_ready;
    std::unordered_map<int, QByteArray> m_mediumPaths;
    std::unordered_set<int> m_mediumAsked;
    std::unordered_set<int> m_mediumMissing;
    bool m_stop = false;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_signalPending{false};
    std::atomic<quint64> m_decoded{0};
    std::atomic<quint64> m_decodeNanos{0};
};
