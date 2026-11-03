#include "framemonitor.h"
#include "framestats.h"

#include <QEvent>
#include <QQuickWindow>
#include <QScreen>

FrameMonitor::FrameMonitor(QQuickWindow *window, FrameStats *stats)
    : QObject(window), m_stats(stats), m_window(window) {
    followScreen(window->screen());
    connect(window, &QWindow::screenChanged, this, [this](QScreen *screen) { followScreen(screen); });
    connect(window, &QQuickWindow::beforeSynchronizing, this, [stats] { stats->syncBegan(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::beforeRendering, this, [stats] { stats->renderBegan(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::afterRendering, this, [stats] { stats->renderEnded(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, [stats] { stats->frameSwapped(); }, Qt::DirectConnection);
    window->installEventFilter(this);
}

void FrameMonitor::followScreen(QScreen *screen) {
    disconnect(m_rateChanged);
    m_stats->setRefreshRate(screen ? screen->refreshRate() : 0);
    // Even two screens of equal rate can have unrelated presentation clocks.
    m_stats->discontinuity();
    if (screen)
        m_rateChanged = connect(screen, &QScreen::refreshRateChanged, this, [this](qreal hz) {
            m_stats->setRefreshRate(hz);
        });
}

bool FrameMonitor::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::Expose)
        m_stats->discontinuity();
    return QObject::eventFilter(watched, event);
}
