#include "scroller.h"

#include "touchpadhold.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace {
QPointer<TouchpadHold> theHold;
// `import Pinhole` has it wherever Pinhole's QML loads (Shield's alike).
void registerScroller() { qmlRegisterType<Scroller>("Pinhole", 1, 0, "Scroller"); }
} // namespace
Q_COREAPP_STARTUP_FUNCTION(registerScroller)

void Scroller::setTouchpadHold(TouchpadHold *hold) { theHold = hold; }

Scroller::Scroller(QQuickItem *parent) : QQuickItem(parent) {
    // The wheel only: presses and touches go on to what is beneath.
    setAcceptedMouseButtons(Qt::NoButton);
    // Coasting into an end it bounces some 45 px, not a screen.
    m_physics.params().bounceVelocity = 1500;
    if (theHold) {
        connect(theHold, &TouchpadHold::began, this, [this](quint32 time) {
            if (isVisible() && m_physics.animating()) {
                m_physics.touch(double(time));
                apply();
            }
        });
    }
}

Scroller::~Scroller() {
    if (m_window)
        m_window->removeEventFilter(this);
}

void Scroller::setFlickable(QQuickItem *flickable) {
    if (flickable == m_flickable)
        return;
    m_flickable = flickable;
    m_physics.stop();
    emit flickableChanged();
}

double Scroller::contentY() const { return m_flickable ? m_flickable->property("contentY").toDouble() : 0; }

void Scroller::syncBounds() {
    if (!m_flickable)
        return;
    const double origin = m_flickable->property("originY").toDouble();
    const double top = origin - m_flickable->property("topMargin").toDouble();
    const double bottom = origin + m_flickable->property("contentHeight").toDouble()
                          + m_flickable->property("bottomMargin").toDouble() - m_flickable->height();
    m_top = top;
    m_bottom = std::max(top, bottom);
    // Pulled past an end, it gives a quarter of its height at most (the
    // grid, a whole screen): a list of settings is not a library to fling.
    m_physics.setBounds(m_top, m_bottom, m_flickable->height() / 4);
}

void Scroller::pickUp() {
    const double y = contentY();
    if ((!m_physics.animating() && !m_physics.tracking()) || std::abs(y - m_written) > 0.5)
        m_physics.jumpTo(y);
    m_written = y;
}

void Scroller::apply() {
    if (!m_flickable)
        return;
    m_written = m_physics.position();
    m_flickable->setProperty("contentY", m_written);
    if (m_physics.animating() && !m_ticking && m_window) {
        m_ticking = true;
        m_frameClock.restart();
        m_window->update();
    }
}

void Scroller::tick() {
    if (!m_ticking)
        return;
    if (theHold)
        theHold->poll();
    // Moved by something else meanwhile (keys, a drag, the list growing
    // under it): that is where it is now.
    if (!m_flickable || std::abs(contentY() - m_written) > 0.5) {
        m_ticking = false;
        m_physics.jumpTo(contentY());
        m_written = contentY();
        return;
    }
    const double dt = std::clamp(m_frameClock.restart() / 1000.0, 0.0, 0.05);
    syncBounds();
    const bool moving = m_physics.step(dt);
    m_written = m_physics.position();
    m_flickable->setProperty("contentY", m_written);
    m_ticking = moving;
    if (moving && m_window)
        m_window->update();
}

bool Scroller::canMove(double dy) const {
    if (!m_flickable || dy == 0)
        return false;
    const double y = contentY();
    const double origin = m_flickable->property("originY").toDouble();
    const double top = origin - m_flickable->property("topMargin").toDouble();
    const double bottom = std::max(top, origin + m_flickable->property("contentHeight").toDouble()
                                            + m_flickable->property("bottomMargin").toDouble() - m_flickable->height());
    // Up the wheel or the fingers, up the content: towards its top.
    return dy > 0 ? y > top + 0.5 : y < bottom - 0.5;
}

Scroller *Scroller::innerAt(QPointF scenePosition, double dy) const {
    QQuickItem *content = m_flickable ? m_flickable->property("contentItem").value<QQuickItem *>() : nullptr;
    if (!content || dy == 0)
        return nullptr;
    // Front to back, as the pointer meets them: the first Scroller there
    // that, or one within it, can still go this way.
    std::function<Scroller *(QQuickItem *)> look = [&](QQuickItem *item) -> Scroller * {
        const QList<QQuickItem *> children = item->childItems();
        for (auto it = children.crbegin(); it != children.crend(); ++it) {
            QQuickItem *child = *it;
            if (!child->isVisible() || !child->isEnabled())
                continue;
            // What a clipped item holds shows only within it.
            if (child->clip() && !child->contains(child->mapFromScene(scenePosition)))
                continue;
            if (auto *inner = qobject_cast<Scroller *>(child)) {
                if (inner->contains(inner->mapFromScene(scenePosition))
                    && (inner->canMove(dy) || inner->innerAt(scenePosition, dy)))
                    return inner;
                continue;
            }
            if (Scroller *found = look(child))
                return found;
        }
        return nullptr;
    };
    return look(content);
}

void Scroller::wheelEvent(QWheelEvent *event) {
    // Every one is its own, whatever it does here: none goes on beneath.
    event->accept();
    take(event);
}

void Scroller::take(const QWheelEvent *event) {
    if (!m_flickable || (event->modifiers() & Qt::ControlModifier))
        return;
    const QPoint pixels = event->pixelDelta(), angle = event->angleDelta();
    const double t = double(event->timestamp());
    const double dy = !pixels.isNull() ? pixels.y() : angle.y() / 8.0;
    // A Scroller within that can still go this way has it first: a swipe,
    // decided at its first movement, its to its end (and the momentum
    // after); a notch, each.
    switch (event->phase()) {
    case Qt::ScrollBegin:
        m_inner = nullptr;
        m_swipeDecided = false;
        m_swipeBegan = event->timestamp();
        break;
    case Qt::ScrollUpdate:
    case Qt::ScrollMomentum:
        if (!m_swipeDecided && dy != 0) {
            m_swipeDecided = true;
            if ((m_inner = innerAt(event->scenePosition(), dy))) {
                // Its, from the fingers landing; this one's still where
                // they landed.
                m_physics.end(t);
                apply();
                QWheelEvent began(event->position(), event->globalPosition(), QPoint(), QPoint(), event->buttons(),
                                  event->modifiers(), Qt::ScrollBegin, event->inverted(), event->source(),
                                  event->pointingDevice());
                began.setTimestamp(m_swipeBegan);
                m_inner->take(&began);
            }
        }
        if (m_inner) {
            m_inner->take(event);
            return;
        }
        break;
    case Qt::ScrollEnd:
        if (m_inner) {
            m_inner->take(event);
            return;
        }
        break;
    case Qt::NoScrollPhase:
        if (std::abs(angle.y()) >= std::abs(angle.x()) && !(angle.isNull() && std::abs(pixels.x()) > std::abs(pixels.y())))
            if (Scroller *inner = innerAt(event->scenePosition(), dy)) {
                inner->take(event);
                return;
            }
        break;
    }
    pickUp();
    syncBounds();
    // As the grid takes them (PhotoGrid::wheelEvent): a touchpad's begin,
    // updates and end, and the momentum a Mac's adds; a wheel's notches
    // glide; pixels without phases are followed as they come.
    switch (event->phase()) {
    case Qt::ScrollBegin:
        if (!m_physics.tracking())
            m_physics.begin(t);
        break;
    case Qt::ScrollUpdate:
    case Qt::ScrollMomentum:
        m_physics.move(t, !pixels.isNull() ? -pixels.y() : -angle.y() / 8.0);
        break;
    case Qt::ScrollEnd:
        m_physics.end(t);
        break;
    case Qt::NoScrollPhase:
        // A sideways wheel: nothing here goes sideways.
        if (std::abs(angle.x()) > std::abs(angle.y()) || (angle.isNull() && std::abs(pixels.x()) > std::abs(pixels.y())))
            return;
        if (angle.y() == 0 && !pixels.isNull()) {
            m_physics.jumpTo(std::clamp(m_physics.position() - pixels.y(), m_top, m_bottom));
        } else {
            m_physics.wheel(t, (!pixels.isNull() ? -pixels.y() / 15.0 : -angle.y() / 120.0) * kNotch);
        }
        break;
    }
    apply();
}

bool Scroller::event(QEvent *event) {
    // A pinch over a panel zooms nothing, the photos beneath included.
    if (event->type() == QEvent::NativeGesture) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

bool Scroller::stopAt(QPointF scenePosition) {
    if (!isVisible() || !isEnabled() || !(m_physics.animating() || m_physics.tracking()))
        return false;
    if (!contains(mapFromScene(scenePosition)))
        return false;
    // Moved by something else meanwhile (a drag, its keys): not this one's
    // to stop.
    if (!m_flickable || std::abs(contentY() - m_written) > 0.5)
        return false;
    // A click or a touch on it while it moves: it stops there, as the grid
    // does (and a two-finger tap is a click of the right button).
    m_physics.stop();
    apply();
    return true;
}

bool Scroller::eventFilter(QObject *watched, QEvent *event) {
    // Seen on its way into the window. Passed on untouched, but for the
    // press or touch that stops it moving: that, and what follows of it to
    // its release, only stops it (a switch under it is not also turned).
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (m_stopped & mouse->button())
            return true;
        if (event->type() == QEvent::MouseButtonPress && stopAt(mouse->scenePosition())) {
            m_stopped |= mouse->button();
            return true;
        }
        break;
    }
    case QEvent::MouseMove:
        if (m_stopped != Qt::NoButton) {
            // A button let go where this never heard it: done with it.
            m_stopped &= static_cast<QMouseEvent *>(event)->buttons();
            return true;
        }
        break;
    case QEvent::MouseButtonRelease:
        if (const auto *mouse = static_cast<QMouseEvent *>(event); m_stopped & mouse->button()) {
            m_stopped &= ~mouse->button();
            return true;
        }
        break;
    case QEvent::TouchBegin:
        if (const auto *touch = static_cast<QTouchEvent *>(event);
            !touch->points().isEmpty() && stopAt(touch->points().first().scenePosition())) {
            m_touchStopped = true;
            return true;
        }
        break;
    case QEvent::TouchUpdate:
        if (m_touchStopped)
            return true;
        break;
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        if (std::exchange(m_touchStopped, false))
            return true;
        break;
    default:
        break;
    }
    return QQuickItem::eventFilter(watched, event);
}

void Scroller::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemSceneChange) {
        if (m_window) {
            m_window->removeEventFilter(this);
            disconnect(m_frames);
        }
        m_window = value.window;
        if (m_window) {
            m_window->installEventFilter(this);
            m_frames = connect(m_window, &QQuickWindow::afterAnimating, this, &Scroller::tick);
        }
    }
    QQuickItem::itemChange(change, value);
}
