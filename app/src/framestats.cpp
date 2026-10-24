#include "framestats.h"

#include <algorithm>

FrameStats::FrameStats() {
    m_clock.start();
    m_samples.reserve(1 << 16);
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
    m_syncAt = m_clock.nsecsElapsed();
}

void FrameStats::renderBegan() {
    std::lock_guard lock(m_mutex);
    m_renderAt = m_clock.nsecsElapsed();
}

void FrameStats::renderEnded() {
    std::lock_guard lock(m_mutex);
    m_renderEndAt = m_clock.nsecsElapsed();
}

void FrameStats::frameSwapped() {
    std::lock_guard lock(m_mutex);
    const qint64 now = m_clock.nsecsElapsed();
    if (m_lastSwap >= 0) {
        Sample s = m_pending;
        s.at = now;
        s.interval = (now - m_lastSwap) / 1e6;
        s.wait = (m_syncAt - m_lastSwap) / 1e6;
        s.render = (m_renderEndAt - m_renderAt) / 1e6;
        s.swap = (now - m_renderEndAt) / 1e6;
        m_samples.push_back(s);
    }
    m_lastSwap = now;
    m_pending = {};
}

void FrameStats::reset() {
    std::lock_guard lock(m_mutex);
    m_samples.clear();
    m_startedAt = m_clock.nsecsElapsed();
    // The first interval after a reset would span the reset itself.
    m_lastSwap = -1;
}

static double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[size_t((v.size() - 1) * p)];
}

FrameStats::Summary FrameStats::summarize(const std::vector<Sample> &samples) {
    Summary s;
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
        if (x.interval > 12.5)
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
        if (x.interval > 12.5)
            worst.push_back(&x);
    std::sort(worst.begin(), worst.end(), [](auto *a, auto *b) { return a->interval > b->interval; });
    for (size_t i = 0; i < std::min<size_t>(3, worst.size()); ++i)
        s.slowest.push_back({worst[i]->interval, worst[i]->wait, worst[i]->sync, worst[i]->upload, worst[i]->evict,
                             worst[i]->render,
                             worst[i]->swap, worst[i]->uploads, worst[i]->evicted, worst[i]->batches});
    return s;
}

FrameStats::Summary FrameStats::summary() {
    std::lock_guard lock(m_mutex);
    return summarize(m_samples);
}

QString FrameStats::overlayText() {
    std::vector<Sample> recent;
    {
        std::lock_guard lock(m_mutex);
        const qint64 since = m_clock.nsecsElapsed() - 1'000'000'000;
        auto it = std::lower_bound(m_samples.begin(), m_samples.end(), since,
                                   [](const Sample &s, qint64 t) { return s.at < t; });
        recent.assign(it, m_samples.end());
        // The overlay only needs the last second; keep the buffer from
        // growing while nobody resets it.
        if (m_samples.size() > 100000)
            m_samples.erase(m_samples.begin(), it);
    }
    const Summary s = summarize(recent);
    return QStringLiteral("%1 fps  frame p50 %2 / p99 %3 / max %4 ms  missed %5\n"
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
        .arg(int(s.batchesMax));
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
