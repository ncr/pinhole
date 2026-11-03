#include "framestats.h"

#include <algorithm>
#include <cmath>

FrameStats::FrameStats(std::function<qint64()> now) : m_now(std::move(now)) {
    m_clock.start();
}

qint64 FrameStats::now() const { return m_now ? m_now() : m_clock.nsecsElapsed(); }

void FrameStats::setRefreshRate(double hz) {
    // Qt reports 60 when the platform cannot supply a rate. Defend against
    // zero/NaN from a disconnected or synthetic screen as well.
    if (!std::isfinite(hz) || hz < 1 || hz > 1000) hz = 60;
    std::lock_guard lock(m_mutex);
    const double period = 1000.0 / hz;
    if (std::abs(period - m_period) < 1e-6) return;
    m_period = period;
    m_lastSwap = -1;
    m_pending = {};
}

void FrameStats::discontinuity() {
    std::lock_guard lock(m_mutex);
    m_lastSwap = -1;
    m_pending = {};
}

void FrameStats::frameBuilt(qint64 syncNanos, int cells, int placeholders, int uploads, int batches, int evicted,
                            qint64 uploadNanos, qint64 evictNanos) {
    std::lock_guard lock(m_mutex);
    m_pending.evict = evictNanos / 1e6;
    m_pending.evicted = evicted;
    m_pending.upload = uploadNanos / 1e6;
    m_pending.sync = syncNanos / 1e6;
    m_pending.cells = cells;
    m_pending.placeholders = placeholders;
    m_pending.uploads = uploads;
    m_pending.batches = batches;
}

void FrameStats::syncBegan() {
    std::lock_guard lock(m_mutex);
    m_syncAt = now();
}

void FrameStats::renderBegan() {
    std::lock_guard lock(m_mutex);
    m_renderAt = now();
}

void FrameStats::renderEnded() {
    std::lock_guard lock(m_mutex);
    m_renderEndAt = now();
}

void FrameStats::frameSwapped() {
    std::lock_guard lock(m_mutex);
    const qint64 at = now();
    if (m_lastSwap >= 0) {
        Sample s = m_pending;
        s.at = at;
        s.period = m_period;
        s.interval = (at - m_lastSwap) / 1e6;
        s.wait = (m_syncAt - m_lastSwap) / 1e6;
        s.render = (m_renderEndAt - m_renderAt) / 1e6;
        s.swap = (at - m_renderEndAt) / 1e6;
        m_samples.push_back(s);
        if (m_samples.size() > (1u << 16))
            m_samples.pop_front();
    }
    m_lastSwap = at;
    m_pending = {};
}

void FrameStats::reset() {
    std::lock_guard lock(m_mutex);
    m_samples.clear();
    m_startedAt = now();
    // The first interval after a reset would span the reset itself.
    m_lastSwap = -1;
}

static double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[size_t((v.size() - 1) * p)];
}

FrameStats::Summary FrameStats::summarize(const std::vector<Sample> &samples, double currentPeriod) {
    Summary s;
    s.targetHz = 1000.0 / currentPeriod;
    s.frames = int(samples.size());
    if (samples.empty())
        return s;
    std::vector<double> intervals, syncs;
    double cells = 0, placeholders = 0, uploads = 0;
    for (const auto &x : samples) {
        intervals.push_back(x.interval);
        syncs.push_back(x.sync);
        cells += x.cells;
        placeholders += x.placeholders;
        uploads += x.uploads;
        s.batchesMax = std::max(s.batchesMax, double(x.batches));
        s.mixedRefreshRates |= std::abs(x.period - currentPeriod) > 1e-6;
        if (x.interval > x.period * 1.5)
            s.missed += 1;
    }
    s.seconds = (samples.back().at - samples.front().at) / 1e9;
    s.intervalP50 = percentile(intervals, 0.5);
    s.intervalP95 = percentile(intervals, 0.95);
    s.intervalP99 = percentile(intervals, 0.99);
    s.intervalMax = *std::max_element(intervals.begin(), intervals.end());
    s.syncP50 = percentile(syncs, 0.5);
    s.syncP99 = percentile(syncs, 0.99);
    s.syncMax = *std::max_element(syncs.begin(), syncs.end());
    s.placeholderShare = cells > 0 ? placeholders / cells : 0;
    s.uploadsPerFrame = uploads / samples.size();
    std::vector<const Sample *> worst;
    for (const auto &x : samples)
        if (x.interval > x.period * 1.5)
            worst.push_back(&x);
    std::sort(worst.begin(), worst.end(), [](auto *a, auto *b) { return a->interval > b->interval; });
    for (size_t i = 0; i < std::min<size_t>(3, worst.size()); ++i)
        s.slowest.push_back({worst[i]->interval, worst[i]->wait, worst[i]->sync, worst[i]->upload, worst[i]->evict,
                             worst[i]->render,
                             worst[i]->swap, worst[i]->uploads, worst[i]->evicted, worst[i]->batches});
    return s;
}

FrameStats::Summary FrameStats::summary() {
    std::vector<Sample> samples;
    double period;
    {
        std::lock_guard lock(m_mutex);
        samples.assign(m_samples.begin(), m_samples.end());
        period = m_period;
    }
    // Percentiles sort copies; never hold the render-thread mutex for it.
    return summarize(samples, period);
}

QString FrameStats::overlayText() {
    std::vector<Sample> recent;
    double period;
    {
        std::lock_guard lock(m_mutex);
        const qint64 since = now() - 1'000'000'000;
        auto it = std::lower_bound(m_samples.begin(), m_samples.end(), since,
                                   [](const Sample &s, qint64 t) { return s.at < t; });
        recent.assign(it, m_samples.end());
        period = m_period;
    }
    const Summary s = summarize(recent, period);
    return QStringLiteral("%1 fps  frame p50 %2 / p99 %3 / max %4 ms  late %5  target %11 Hz%12\n"
                          "sync p50 %6 / max %7 ms  placeholders %8%  uploads %9/frame  batches %10")
        .arg(s.frames)
        .arg(s.intervalP50, 0, 'f', 1)
        .arg(s.intervalP99, 0, 'f', 1)
        .arg(s.intervalMax, 0, 'f', 1)
        .arg(s.missed)
        .arg(s.syncP50, 0, 'f', 2)
        .arg(s.syncMax, 0, 'f', 2)
        .arg(int(s.placeholderShare * 100))
        .arg(s.uploadsPerFrame, 0, 'f', 1)
        .arg(int(s.batchesMax))
        .arg(s.targetHz, 0, 'f', 2)
        .arg(s.mixedRefreshRates ? QStringLiteral(" (mixed)") : QString());
}

QString FrameStats::aroundWorst(int before, int after) {
    std::lock_guard lock(m_mutex);
    if (m_samples.empty())
        return {};
    const auto worst = std::max_element(m_samples.begin(), m_samples.end(),
                                        [](const Sample &a, const Sample &b) { return a.interval < b.interval; });
    const long at = worst - m_samples.begin();
    QString out;
    for (long i = std::max(0L, at - before); i <= std::min(long(m_samples.size()) - 1, at + after); ++i) {
        const Sample &x = m_samples[size_t(i)];
        out += QStringLiteral("    %1 frame %2 ms: wait %3 sync %4 render %5 swap %6  uploads %7 evicted %8 ph %9/%10\n")
                   .arg(i == at ? ">" : " ")
                   .arg(x.interval, 5, 'f', 1)
                   .arg(x.wait, 4, 'f', 1)
                   .arg(x.sync, 5, 'f', 2)
                   .arg(x.render, 5, 'f', 1)
                   .arg(x.swap, 5, 'f', 1)
                   .arg(x.uploads, 3)
                   .arg(x.evicted, 3)
                   .arg(x.placeholders, 3)
                   .arg(x.cells, 3);
    }
    return out;
}
