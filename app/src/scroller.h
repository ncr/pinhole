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
// go no further, and the photos under the panel scrolled. Presses, clicks,
// hovers and touches it leaves to the Flickable and what is in it (a touch
// drag still drags the Flickable); whoever else moves the Flickable (its
// keys, a drag) it takes up from where they left it.
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
    void stopAt(QPointF scenePosition);

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
};
