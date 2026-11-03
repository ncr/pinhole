#pragma once

#include <QElapsedTimer>
#include <QString>

#include <functional>
#include <mutex>
#include <deque>
#include <vector>

// Frame timing as the render thread sees it: the interval between buffer
// swaps and the grid's scene graph work. Each sample carries the screen's
// target period: an interval over 1.5 periods is a late frame. Screen
// changes and exposure discontinuities never turn a pause into a late frame.
class FrameStats {
public:
    explicit FrameStats(std::function<qint64()> now = {});

    void setRefreshRate(double hz);
    // Keep the history but discard the interval spanning an exposure or
    // refresh-rate change. Thread safe, like the render callbacks below.
    void discontinuity();

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
        double targetHz = 60;
        bool mixedRefreshRates = false;
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
        double period;
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
    static Summary summarize(const std::vector<Sample> &samples, double currentPeriod);
    qint64 now() const;

    std::mutex m_mutex;
    QElapsedTimer m_clock;
    std::function<qint64()> m_now;
    double m_period = 1000.0 / 60;
    qint64 m_lastSwap = -1;
    qint64 m_syncAt = 0, m_renderAt = 0, m_renderEndAt = 0;
    qint64 m_startedAt = 0;
    Sample m_pending{};
    // Bounded even with the overlay closed: about nine minutes at 120 Hz,
    // far more than a benchmark phase. No periodic large vector compaction.
    std::deque<Sample> m_samples;
};
