#pragma once

#include <QObject>

class FrameStats;
class QQuickWindow;
class QScreen;

// Window lifecycle and display timing belong together, outside main's
// service/library wiring. Owned by the window; FrameStats outlives it.
class FrameMonitor : public QObject {
public:
    FrameMonitor(QQuickWindow *window, FrameStats *stats);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void followScreen(QScreen *screen);
    FrameStats *m_stats;
    QQuickWindow *m_window;
    QMetaObject::Connection m_rateChanged;
};
