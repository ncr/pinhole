#pragma once

#include <QElapsedTimer>
#include <QString>

#include <mutex>
#include <deque>
#include <vector>

// Frame timing as the render thread sees it: the interval between buffer
// swaps, and how long the grid's scene graph update took. At 120 Hz a
// frame has 8.33 ms; an interval over 12.5 ms means at least one vsync was
// missed.
class FrameStats {
public:
    FrameStats();

    // Render thread, in frame order: sync begins (the GUI thread has handed
    // over the frame), rendering begins and ends, the buffer is swapped.
    void syncBegan();
    void renderBegan();
    void renderEnded();
    void frameSwapped();
    void frameBuilt(qint64 syncNanos, int cells, int placeholders, int uploads, int batches, int evicted,
                    qint64 uploadNanos, qint64 evictNanos);

    struct Summary {
        int frames = 0;
        double seconds = 0;
        double intervalP50 = 0, intervalP95 = 0, intervalP99 = 0, intervalMax = 0;
        int missed = 0;
        double syncP50 = 0, syncP99 = 0, syncMax = 0;
        double placeholderShare = 0; // of all cells drawn
        double uploadsPerFrame = 0;
        double batchesMax = 0;
        // The worst frames, to tell what made them slow.
        struct Slow { double interval, wait, sync, upload, evict, render, swap; int uploads, evicted, batches; };
        std::vector<Slow> slowest;
    };

    void reset();
    Summary summary();
    // The frames around the slowest one, one line each, for tracing what
    // led up to it.
    QString aroundWorst(int before, int after);
    // The last second, for the on-screen overlay.
    QString overlayText();

private:
    struct Sample {
        qint64 at;
        double interval;
        double sync;
        int cells;
        int placeholders;
        int uploads;
        int batches;
        int evicted;
        double upload;
        double evict;
        // Milliseconds from the previous swap to sync (the GUI thread and
        // vsync), of rendering, and from the end of rendering to the swap.
        double wait;
        double render;
        double swap;
    };
    Summary summarize(const std::vector<Sample> &samples);

    std::mutex m_mutex;
    QElapsedTimer m_clock;
    qint64 m_lastSwap = -1;
    qint64 m_syncAt = 0, m_renderAt = 0, m_renderEndAt = 0;
    qint64 m_startedAt = 0;
    Sample m_pending{};
    // Bounded even with the overlay closed: about nine minutes at 120 Hz,
    // far more than a benchmark phase. No periodic large vector compaction.
    std::deque<Sample> m_samples;
};
