#include "bench.h"

#include "framestats.h"
#include "photogrid.h"
#include "thumbnailloader.h"

#include <QDateTime>
#include <QImage>
#include <QQuickWindow>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
// The first quarter second of a phase settles the change of pace; it is not
// measured.
constexpr double kWarmup = 0.25;
} // namespace

Bench::Bench(PhotoGrid *grid, FrameStats *stats, ThumbnailLoader *loader, QObject *parent)
    : QObject(parent), m_grid(grid), m_stats(stats), m_loader(loader) {
    PhotoGrid *g = m_grid;
    auto centre = [g] { return QPointF(g->width() / 2, g->height() / 2); };
    auto stopped = [g] {
        g->setDriveVelocity(0);
        g->fling(0);
    };
    // The stride at a column count, for the zoom sweep.
    auto strideAt = [g](double columns) { return g->width() / columns; };

    m_phases = {
        {"idle at the newest", 2.0, [g, stopped] { stopped(); g->scrollToEnd(); }, {}},
        {"scroll 1,500 px/s", 4.0, [g] { g->setDriveVelocity(-1500); }, {}},
        {"scroll 5,000 px/s", 4.0, [g] { g->setDriveVelocity(-5000); }, {}},
        {"scroll 15,000 px/s", 4.0, [g] { g->setDriveVelocity(-15000); }, {}},
        {"three flings", 6.0, [g, stopped] { stopped(); g->jumpToFraction(0.6); },
         [g](double before, double t) {
             // A strong flick every two seconds.
             if (std::floor(t / 2) != std::floor(before / 2) || before == 0)
                 g->fling(-30000);
         }},
        {"scrub the whole library in 5 s", 5.0,
         [g, stopped] {
             stopped();
             g->scrollToEnd();
             g->setDriveVelocity(-g->scrollRange() / 5.0);
         },
         {}},
        {"jump through the library by position, newest to oldest", 4.5,
         [g, stopped] {
             stopped();
             g->setScrubbing(true);
             const QVariantList marks = g->yearMarks();
             std::printf("%-40s %lld year marks; at 0.25 the scrubber says %s, at 0.75 %s\n", "", qlonglong(marks.size()),
                         qPrintable(QDateTime::fromMSecsSinceEpoch(g->dateAt(0.25)).toString("MMMM yyyy")),
                         qPrintable(QDateTime::fromMSecsSinceEpoch(g->dateAt(0.75)).toString("MMMM yyyy")));
         },
         [g](double, double t) {
             g->scrubTo(1 - std::min(1.0, t / 4.0));
             if (t >= 4.4)
                 g->setScrubbing(false);
         }},
        {"random jumps", 8.0, [stopped] { stopped(); },
         [g, this](double before, double t) {
             if (m_jumpAt >= 0 && g->unresolvedVisible() == 0 && t - m_jumpAt > 1.0 / 60) {
                 m_jumpTimes.push_back(t - m_jumpAt);
                 m_jumpAt = -1;
             }
             if (std::floor(t / 0.8) != std::floor(before / 0.8) || before == 0) {
                 if (m_jumpAt >= 0)
                     m_jumpTimes.push_back(t - m_jumpAt); // never resolved: counts its whole slot
                 g->jumpToFraction(QRandomGenerator::global()->generateDouble());
                 m_jumpAt = t;
             }
         }},
        {"pinch sweep 7 -> 29 -> 3 -> 7 columns", 6.5, [g, stopped] { stopped(); g->jumpToFraction(0.5); },
         [g, centre, strideAt](double, double t) {
             // Interpolate the column count logarithmically, the way a pinch
             // feels: equal finger travel, equal ratio.
             auto lerpLog = [](double a, double b, double f) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * f); };
             double columns;
             if (t < 2.0)
                 columns = lerpLog(7, 29, t / 2.0);
             else if (t < 4.5)
                 columns = lerpLog(29, 3, (t - 2.0) / 2.5);
             else if (t < 6.0)
                 columns = lerpLog(3, 7, (t - 4.5) / 1.5);
             else {
                 g->endContinuousZoom();
                 return;
             }
             g->setEffectiveStride(strideAt(columns), centre());
         }},
        {"pinch at the newest 7 -> 29 -> 1 -> 7", 6.5, [g, stopped] { stopped(); g->scrollToEnd(); },
         [g, strideAt](double, double t) {
             // Around a point near the bottom, where the library ends.
             const QPointF at(g->width() * 0.6, g->height() * 0.85);
             auto lerpLog = [](double a, double b, double f) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * f); };
             double columns;
             if (t < 2.0)
                 columns = lerpLog(7, 29, t / 2.0);
             else if (t < 4.5)
                 columns = lerpLog(29, 1, (t - 2.0) / 2.5);
             else if (t < 6.0)
                 columns = lerpLog(1, 7, (t - 4.5) / 1.5);
             else {
                 g->endContinuousZoom();
                 return;
             }
             g->setEffectiveStride(strideAt(columns), at);
         }},
        {"idle at 1 column (sharp photos load)", 8.0,
         [g, stopped] {
             stopped();
             g->setColumnsNow(1);
             g->jumpToFraction(0.97);
         },
         {}},
        {"idle at 29 columns (fully out)", 3.0,
         [g, stopped] {
             stopped();
             g->setColumnsNow(29);
         },
         {}},
        {"scroll 3,000 px/s at 29 columns", 4.0, [g] { g->setDriveVelocity(-3000); }, {}},
        {"scroll 15,000 px/s at 29 columns", 4.0, [g] { g->setDriveVelocity(-15000); }, {}},
        {"zoom steps 7 -> 29 -> 7, animated", 5.0,
         [g, stopped] {
             stopped();
             g->setColumnsNow(7);
         },
         [g](double before, double t) {
             // A level every 0.4 s, two out and two back in.
             if (std::floor(t / 0.4) != std::floor(before / 0.4)) {
                 const int step = int(t / 0.4);
                 g->zoomStep(step % 4 < 2 ? +1 : -1);
             }
         }},
    };
    // PINHOLE_BENCH_ONLY=<text>: only the phases whose name has it.
    if (const QString only = qEnvironmentVariable("PINHOLE_BENCH_ONLY"); !only.isEmpty())
        m_phases.erase(std::remove_if(m_phases.begin(), m_phases.end(),
                                      [&only](const Phase &p) { return !p.name.contains(only); }),
                       m_phases.end());
    // PINHOLE_BENCH_IDLE=<seconds>: only redraw the same frame, to see how
    // often the system alone stalls a frame.
    if (const int idle = qEnvironmentVariableIntValue("PINHOLE_BENCH_IDLE"); idle > 0)
        m_phases = {{QStringLiteral("baseline: redraw, nothing changes"), double(idle), [g, stopped] { stopped(); }, {}}};
}

void Bench::start() {
    m_running = true;
    m_current = 0;
    emit runningChanged();
    std::printf("pinhole bench: %d items, window %.0fx%.0f logical\n", m_grid->count(), m_grid->width(),
                m_grid->height());
    std::printf("%-40s %6s %6s %6s %6s %6s %7s %7s %7s %6s %6s %6s %7s\n", "phase", "fps", "p50", "p95", "p99", "max",
                "missed", "sync50", "sync99", "ph%", "upl/f", "batch", "dec/s");
    std::fflush(stdout);
    beginPhase();
}

void Bench::beginPhase() {
    m_grid->takePinJump();
    m_grid->takeUncovered();
    m_grid->takeCellOffset();
    m_t = 0;
    m_warm = false;
    m_jumpAt = -1;
    m_jumpTimes.clear();
    const Phase &p = m_phases[m_current];
    if (p.begin)
        p.begin();
    m_stats->reset();
    m_decodedAtStart = m_loader->decodedCount();
}

void Bench::tick(qreal dt) {
    if (!m_running)
        return;
    const double before = m_t;
    m_t += dt;
    const Phase &p = m_phases[m_current];
    if (!m_warm && m_t >= kWarmup) {
        m_warm = true;
        m_stats->reset();
        m_decodedAtStart = m_loader->decodedCount();
    }
    if (p.step)
        p.step(before, m_t);
    // PINHOLE_BENCH_GRAB=<folder>: the window 20 times a second, to look at
    // what a phase looks like (grabbing stalls frames: not for timing).
    static const QString grabs = qEnvironmentVariable("PINHOLE_BENCH_GRAB");
    if (!grabs.isEmpty() && std::floor(m_t * 20) != std::floor(before * 20)) {
        static int n = 0;
        m_grid->window()->grabWindow().save(QStringLiteral("%1/%2.png").arg(grabs).arg(++n, 4, 10, QLatin1Char('0')));
    }
    if (m_t >= p.seconds)
        endPhase();
}

void Bench::endPhase() {
    const Phase &p = m_phases[m_current];
    const FrameStats::Summary s = m_stats->summary();
    const double measured = std::max(1e-3, p.seconds - kWarmup);
    std::printf("%-40s %6.0f %6.1f %6.1f %6.1f %6.1f %4d/%-3.0f%% %6.2f %6.2f %6.0f %6.1f %6.0f %7.0f\n",
                p.name.toUtf8().constData(), s.frames / std::max(1e-3, s.seconds), s.intervalP50, s.intervalP95,
                s.intervalP99, s.intervalMax, s.missed, s.frames ? 100.0 * s.missed / s.frames : 0, s.syncP50, s.syncP99,
                s.placeholderShare * 100, s.uploadsPerFrame, s.batchesMax,
                (m_loader->decodedCount() - m_decodedAtStart) / measured);
    for (const auto &w : s.slowest)
        std::printf("%-40s slow frame %.1f ms: wait %.1f, sync %.2f (uploads %d in %.2f, evicted %d in %.2f), "
                    "render %.2f, swap %.1f; batches %d\n",
                    "", w.interval, w.wait, w.sync, w.uploads, w.upload, w.evicted, w.evict, w.render, w.swap, w.batches);
    if (p.name.contains("sharp")) {
        const auto [needed, shown] = m_grid->mediumOnScreen();
        std::printf("%-40s %d of %d cells on screen show the medium rendition\n", "", shown, needed);
    }
    if (p.name.contains("pinch") || p.name.contains("zoom steps")) {
        const auto [frames, band] = m_grid->takeUncovered();
        std::printf("%-40s pinned photo moved up or down at most %.1f px in one frame; %d frames with an empty edge (widest %.0f px)\n",
                    "", m_grid->takePinJump(), frames, band);
        const auto [fading, columns, rows] = m_grid->takeCellOffset();
        std::printf("%-40s %d frames cross-fading two levels; their cells at most %.1f px apart across, %.1f px down\n",
                    "", fading, columns, rows);
    }
    if (qEnvironmentVariableIsSet("PINHOLE_TRACE") && s.intervalMax > 25)
        std::printf("%s", qPrintable(m_stats->aroundWorst(10, 3)));
    if (!m_jumpTimes.empty()) {
        auto v = m_jumpTimes;
        std::sort(v.begin(), v.end());
        std::printf("%-40s %zu jumps; all visible cells sharp after p50 %.0f ms, max %.0f ms\n", "",
                    v.size(), v[v.size() / 2] * 1000, v.back() * 1000);
    }
    std::fflush(stdout);
    m_current += 1;
    if (m_current < m_phases.size()) {
        beginPhase();
        return;
    }
    m_grid->setDriveVelocity(0);
    m_running = false;
    emit runningChanged();
    emit finished();
}
