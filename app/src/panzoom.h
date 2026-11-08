#pragma once

#include "scrollphysics.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickItem>
#include <deque>

class QNativeGestureEvent;

// A surface moved and zoomed without levels (a map, a picture larger than
// its room), with the grid's physics: what a plugin's own camera
// follows. It says how far to move and zoom, the plugin moves its camera:
//
// - a touchpad's swipe pans, following the fingers and coasting on as they
//   lift, as the grid scrolls; fingers coming to rest, or a click, stop it;
// - a drag with the mouse (the left or middle button) pans one to one, and
//   coasts on if it was let go moving;
// - the wheel, and Ctrl with a touchpad's swipe, zoom around the pointer,
//   gliding notch by notch as the grid's wheel scrolls; a touchpad's pinch
//   zooms with the fingers around them;
// - a click on it (none of its children's) is tapped(), a double click
//   doubleTapped(): zoom in there, say.
//
// Every wheel turn, swipe and pinch over it is its own, so none reaches the
// photos beneath; over what is over it (a panel of Pinhole's), none is.
// Its children (markers) take their own presses.
class PanZoom : public QQuickItem {
    Q_OBJECT
    // How far a wheel's notch zooms, in steps of 2 (0.5: a notch is a
    // square root of two closer).
    Q_PROPERTY(qreal zoomPerNotch READ zoomPerNotch WRITE setZoomPerNotch NOTIFY zoomPerNotchChanged)
    // Panning or zooming now, by the fingers, the mouse or a glide.
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
public:
    explicit PanZoom(QQuickItem *parent = nullptr);
    ~PanZoom() override;

    qreal zoomPerNotch() const { return m_zoomPerNotch; }
    void setZoomPerNotch(qreal steps);
    bool moving() const;
    bool dragging() const { return m_dragging; }

    // Stop all motion where it is.
    Q_INVOKABLE void stop();
    // Zoom by `steps` (of 2) around (x, y), gliding there as a notch does.
    Q_INVOKABLE void zoomBy(qreal x, qreal y, qreal steps);

    // One frame, `dt` seconds (the window's frames, a self-test's alike).
    bool advance(double dt);

signals:
    // Move the content by (dx, dy) pixels: right and down are positive.
    void panned(qreal dx, qreal dy);
    // Zoom by `steps` of 2 (positive: in) around (x, y), in its coordinates.
    void zoomed(qreal x, qreal y, qreal steps);
    void tapped(qreal x, qreal y);
    void doubleTapped(qreal x, qreal y);
    void zoomPerNotchChanged();
    void movingChanged();
    void draggingChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void takeWheel(const QWheelEvent *event, QPointF at);
    void takeGesture(const QNativeGestureEvent *event, QPointF at);
    void run();
    void tick();
    void settle();

    // Panning: content pixels, as a Scroller's; zooming: a notch is
    // kNotch of its own.
    static constexpr double kNotch = 180;
    ScrollPhysics m_x, m_y, m_zoom;
    double m_xAt = 0, m_yAt = 0, m_zoomAt = 0;
    QPointF m_zoomAround;
    qreal m_zoomPerNotch = 0.5;

    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_frames;
    bool m_ticking = false;
    bool m_wasMoving = false;
    QElapsedTimer m_frameClock;

    // A drag with the mouse: where it was, and its recent moves for the
    // speed it is let go at.
    bool m_pressed = false, m_dragging = false, m_stoppedIt = false;
    QPointF m_pressAt, m_last;
    struct Sample { qint64 t; QPointF at; };
    std::deque<Sample> m_samples;
    QElapsedTimer m_clock;
};
