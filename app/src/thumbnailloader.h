#pragma once

#include <QImage>
#include <QObject>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

class Library;

// A thumbnail at one of four decode levels: 0 is the stored size, then 1/2,
// 1/4 and 1/8, which libjpeg-turbo produces while decoding at a fraction of
// the cost.
inline quint64 thumbKey(int index, int level) { return (quint64(index) << 2) | quint64(level); }
inline int keyIndex(quint64 key) { return int(key >> 2); }
inline int keyLevel(quint64 key) { return int(key & 3); }

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

    int threads() const { return int(m_threads.size()); }
    quint64 decodedCount() const { return m_decoded.load(); }
    quint64 decodeNanos() const { return m_decodeNanos.load(); }
    size_t queued();

signals:
    // Emitted from a worker thread, at most once until takeReady().
    void ready();

private:
    void run();

    const Library *m_library;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<quint64> m_queue;
    std::unordered_set<quint64> m_inFlight;
    std::unordered_set<quint64> m_readyKeys;
    // Files that would not decode; asked for again they would be retried
    // every frame.
    std::unordered_set<quint64> m_failed;
    std::vector<DecodedThumb> m_ready;
    bool m_stop = false;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_signalPending{false};
    std::atomic<quint64> m_decoded{0};
    std::atomic<quint64> m_decodeNanos{0};
};
