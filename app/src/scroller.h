#pragma once

#include "scrollphysics.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickItem>

class TouchpadHold;
class Zoomer;

// The wheel and the touchpad over a Flickable (a plugin's list, grid or
// scroll area; Pinhole's side panel, help and pickers), scrolling it
// exactly as the grid scrolls (ScrollPhysics, PhotoGrid::wheelEvent), with
// the grid's notch, rubber band and bounce: the content follows the fingers
// and coasts on when they lift, fingers resting or a click stop it, a
// wheel's notches glide, and the ends give and spring back. Up and down,
// and across too where the content is wider than the view: a sideways
// swipe, the sideways wheel, and the wheel itself over content that only
// goes across.
//
// Declared inside its Flickable (ScrollList.qml, ScrollGrid.qml and
// ScrollArea.qml are each one with a Scroller in it), it finds it and lays
// itself over it, as large as it, above what it shows; or it is laid over
// one and given it as `flickable`. It takes every wheel turn, swipe and
// pinch over it, at the ends too, so none goes on to what is beneath; a
// Flickable left them through once it could go no further, and the photos
// under the panel scrolled. A pinch, and Ctrl or the sideways wheel where
// nothing goes across, go to the PinchZoom it is in, if any. Another
// Scroller within its Flickable (a plugin's list in its settings) has them
// first while it, or one within it, can still go the way they go: a swipe,
// decided at its first movement, to its end; each notch; then this one.
// Presses, clicks, hovers and touches it leaves to the Flickable and what
// is in it (a touch drag still drags the Flickable), but for the one that
// stops it moving: that press and its release, or that touch, only stop it,
// as on the grid, and never also press what was under them. Whoever else
// moves the Flickable (its keys, a drag, positionViewAtIndex) it takes up
// from where they left it.
class Scroller : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QQuickItem *flickable READ flickable WRITE setFlickable NOTIFY flickableChanged)
public:
    explicit Scroller(QQuickItem *parent = nullptr);
    ~Scroller() override;

    // Fingers coming to rest on the touchpad stop a coast, as on the grid.
    static void setTouchpadHold(TouchpadHold *hold);
    static TouchpadHold *touchpadHold();

    QQuickItem *flickable() const { return m_flickable; }
    void setFlickable(QQuickItem *flickable);

    // One frame of its motion, `dt` seconds: the window's frames drive it,
    // and the physics self-test, frame by frame, alike. True while moving.
    bool advance(double dt);
    bool moving() const;

    // Pixels a wheel's notch moves, per pixel Wayland gives for it (15 a
    // notch) or per eighth of a degree: the grid's 12, 180 px a notch.
    static constexpr double kGridWheel = 12;

signals:
    void flickableChanged();

protected:
    void componentComplete() override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    // Up and down (contentY) or across (contentX): its ends, its physics,
    // and where it last put the content.
    struct Axis {
        const char *position, *origin, *content, *before, *after;
        bool across;
        ScrollPhysics physics;
        double low = 0, high = 0, written = 0;
    };
    double at(const Axis &axis) const;
    double view(const Axis &axis) const;
    void syncBounds(Axis &axis);
    bool scrolls(const Axis &axis) const;
    // Where the Flickable is, taken up if anyone else moved it.
    void pickUp(Axis &axis);
    // The positions to the Flickable; frames asked for while it moves.
    void apply();
    void tick();
    void adopt();
    void fit();
    // A wheel event, as over it, whoever it came to.
    void take(const QWheelEvent *event);
    // True if it was moving there and stopped.
    bool stopAt(QPointF scenePosition);
    // Its Flickable can still move by `d` (as the wheel gives it: up, or
    // left, is positive) across or down.
    bool canMove(bool across, double d) const;
    // The Scroller within its Flickable under `scenePosition` that, or one
    // within which, can still move by `d`; null if none.
    Scroller *innerAt(QPointF scenePosition, bool across, double d) const;
    // The PinchZoom it is in, if any.
    Zoomer *zoomer() const;

    QPointer<QQuickItem> m_flickable;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_frames;
    QList<QMetaObject::Connection> m_fitting;
    Axis m_y{"contentY", "originY", "contentHeight", "topMargin", "bottomMargin", false, {}};
    Axis m_x{"contentX", "originX", "contentWidth", "leftMargin", "rightMargin", true, {}};
    bool m_ticking = false;
    QElapsedTimer m_frameClock;
    // The buttons whose press stopped it: their moves and release go
    // nowhere either. And the touch that did.
    Qt::MouseButtons m_stopped;
    bool m_touchStopped = false;
    // A touchpad's swipe goes to what is under it at its first movement,
    // to its end (and the momentum after): an inner Scroller, or this one
    // (null). Until then, not decided.
    QPointer<Scroller> m_inner;
    bool m_swipeDecided = true;
    quint64 m_swipeBegan = 0;
};
