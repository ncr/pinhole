#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <vector>

class PhotoGrid;
class FrameStats;
class ThumbnailLoader;

// A scripted run through the grid: scrolling at several speeds, flings, a
// scrub through the whole library, random jumps and zooming. Each phase
// reports frame timing; the whole run prints one line per phase and quits.
class Bench : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    Bench(PhotoGrid *grid, FrameStats *stats, ThumbnailLoader *loader, QObject *parent = nullptr);

    bool running() const { return m_running; }
    void start();
    // Called every frame while running, before the grid advances.
    Q_INVOKABLE void tick(qreal dt);

signals:
    void runningChanged();
    void finished();

private:
    struct Phase {
        QString name;
        double seconds;
        std::function<void()> begin;
        // Each frame, with the phase time before and after it.
        std::function<void(double before, double t)> step;
    };
    void beginPhase();
    void endPhase();

    PhotoGrid *m_grid;
    FrameStats *m_stats;
    ThumbnailLoader *m_loader;
    std::vector<Phase> m_phases;
    size_t m_current = 0;
    double m_t = 0;
    bool m_running = false;
    bool m_warm = false;
    quint64 m_decodedAtStart = 0;

    // Jumps: when each happened and how long until every visible cell was
    // sharp.
    double m_jumpAt = -1;
    std::vector<double> m_jumpTimes;
};
