#pragma once

#include "scrollphysics.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickItem>

class TouchpadHold;

// The wheel and the touchpad over a Flickable (a panel's or a card's list),
// scrolling it as the grid scrolls (ScrollPhysics): the content follows the
// fingers and coasts on when they lift, fingers resting or a click stop it,
// a wheel's notches glide, and the ends give a little and spring back.
//
// Laid over what it scrolls, as large as the card or panel around it, it
// takes every wheel turn, swipe and pinch over it, at the ends too, so none
// goes on to what is beneath; a Flickable left them through once it could
// go no further, and the photos under the panel scrolled. Another Scroller
// within its Flickable (over a plugin's list in its settings) has them
// first while it, or one within it, can still go the way they go: a swipe,
// decided at its first movement, to its end; each notch; then this one.
// Presses, clicks, hovers and touches it leaves to the Flickable and what
// is in it (a touch drag still drags the Flickable), but for the one that
// stops it moving: that press and its release, or that touch, only stop it,
// as on the grid, and never also press what was under them. Whoever else
// moves the Flickable (its keys, a drag) it takes up from where they left
// it.
class Scroller : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QQuickItem *flickable READ flickable WRITE setFlickable NOTIFY flickableChanged)
public:
    explicit Scroller(QQuickItem *parent = nullptr);
    ~Scroller() override;

    // Fingers coming to rest on the touchpad stop a coast, as on the grid.
    static void setTouchpadHold(TouchpadHold *hold);

    QQuickItem *flickable() const { return m_flickable; }
    void setFlickable(QQuickItem *flickable);

    // Pixels a mouse wheel's notch moves (a notch of 120, or Wayland's 15
    // pixels): about three lines of a panel's text.
    static constexpr double kNotch = 120;

signals:
    void flickableChanged();

protected:
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    double contentY() const;
    void syncBounds();
    // Where the Flickable is, taken up if anyone else moved it.
    void pickUp();
    // The position to the Flickable; frames asked for while it moves.
    void apply();
    void tick();
    // A wheel event, as over it, whoever it came to.
    void take(const QWheelEvent *event);
    // True if it was moving there and stopped.
    bool stopAt(QPointF scenePosition);
    // Its Flickable can still move by `dy` (as the wheel gives it: up is
    // positive).
    bool canMove(double dy) const;
    // The Scroller within its Flickable under `scenePosition` that, or one
    // within which, can still move by `dy`; null if none.
    Scroller *innerAt(QPointF scenePosition, double dy) const;

    QPointer<QQuickItem> m_flickable;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_frames;
    ScrollPhysics m_physics;
    // Its ends, as contentY: the first line at the top, the last at the
    // foot.
    double m_top = 0, m_bottom = 0;
    // The contentY last set here; any other means someone else moved it.
    double m_written = 0;
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
