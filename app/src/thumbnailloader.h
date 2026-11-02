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

// The medium rendition (2048 px, from the source through pinholed) at a decode
// level, for cells too big for the 480 px thumbnail. Marked by bit 62.
inline quint64 mediumKey(int index, int level) { return (quint64(1) << 62) | thumbKey(index, level); }
inline bool isMediumKey(quint64 key) { return (key >> 62) == 1; }
// A full photo, decoded only far enough for its square grid crop. The
// levels request short edges of 8192, 4096, 2048 or 1024 device pixels.
inline quint64 fullKey(int index, int level) { return (quint64(3) << 62) | thumbKey(index, level); }
inline bool isFullKey(quint64 key) { return (key >> 62) == 3; }
inline bool isDetailKey(quint64 key) { return isMediumKey(key) || isFullKey(key); }
inline int fullShortEdge(int level) { return 8192 >> level; }
inline int fullLevelFor(double needed) {
    int level = 0;
    while (level < 3 && fullShortEdge(level + 1) >= needed) ++level;
    return level;
}

// A mosaic page: 256 consecutive photos as 64 px squares, 16 by 16, in one
// 1024 px image. When cells are this small, one page replaces 256 decodes
// and 256 texture uploads, and on Asahi it is the count of uploads, not
// their size, that stalls the GPU. Pages share the key space with
// thumbnails, marked by the top bit.
constexpr int kMosaicTile = 64;
constexpr int kMosaicSide = 16;
constexpr int kMosaicCount = kMosaicSide * kMosaicSide;
inline quint64 pageKey(int page) { return (quint64(1) << 63) | quint64(page); }
inline bool isPageKey(quint64 key) { return (key >> 62) == 2; }
inline int keyPage(quint64 key) { return int(key & ~(quint64(1) << 63)); }

// One JPEG as the loader decodes it, at 1/(2^level) of its size, in sRGB
// (for tests).
QImage decodeThumbnail(const QByteArray &path, int level);

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
    // that it has none. Main thread. `generation` is the one it was asked
    // for in (mediumNeeded): an answer from before a reload is dropped, as
    // its position means another photo now.
    void setMediumPath(int index, const QByteArray &path, quint64 generation);
    void setMediumUnavailable(int index, quint64 generation);
    // Forget about a photo's medium rendition, so it is asked for again.
    void clearMedium(int index, quint64 generation);
    bool mediumUnavailable(int index);
    void setFullPath(int index, const QByteArray &path, quint64 generation);
    void setFullUnavailable(int index, quint64 generation);
    void clearFull(int index, quint64 generation);
    // Counts reloads: positions keep their meaning within one generation.
    quint64 generation() const { return m_generation; }

    // Run `swap` with every decoder idle, then forget everything keyed by
    // position (queued, decoded, failed, medium files): the positions mean
    // other photos now. GUI thread.
    void reload(const std::function<void()> &swap);

    int threads() const { return int(m_threads.size()); }
    quint64 decodedCount() const { return m_decoded.load(); }
    quint64 decodeNanos() const { return m_decodeNanos.load(); }
    // Of one kind (0 thumbnails, 1 detail renditions, 2 mosaic pages): how
    // many were decoded, and in how long all told (each on its thread).
    quint64 decodedCount(int kind) const { return m_kindCount[kind].load(); }
    quint64 decodeNanos(int kind) const { return m_kindNanos[kind].load(); }
    size_t queued();

signals:
    // Emitted from a worker thread, at most once until takeReady().
    void ready();
    // Medium renditions wanted whose files are not known yet: ask pinholed.
    // Emitted from the render thread; `generation` as in setMediumPath.
    void mediumNeeded(const QList<int> &indexes, quint64 generation);
    void fullNeeded(const QList<int> &indexes, quint64 generation);

private:
    void run(bool fullDecoder);

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
    std::unordered_map<int, QByteArray> m_fullPaths;
    std::unordered_set<int> m_fullAsked;
    std::unordered_set<int> m_fullMissing;
    // Read under m_mutex off the main thread; written there under it too.
    quint64 m_generation = 0;
    int m_fullActive = 0; // cap native-photo decodes at two, even with many thumbnail workers
    bool m_stop = false;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_signalPending{false};
    std::atomic<quint64> m_decoded{0};
    std::atomic<quint64> m_decodeNanos{0};
    std::atomic<quint64> m_kindCount[3] = {0, 0, 0};
    std::atomic<quint64> m_kindNanos[3] = {0, 0, 0};
};
