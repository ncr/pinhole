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

void Scroller::wheelEvent(QWheelEvent *event) {
    // Every one is its own, whatever it does here: none goes on beneath.
    event->accept();
    if (!m_flickable || (event->modifiers() & Qt::ControlModifier))
        return;
    const QPoint pixels = event->pixelDelta(), angle = event->angleDelta();
    const double t = double(event->timestamp());
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

void Scroller::stopAt(QPointF scenePosition) {
    if (!isVisible() || !isEnabled() || !(m_physics.animating() || m_physics.tracking()))
        return;
    if (!contains(mapFromScene(scenePosition)))
        return;
    // A click or a touch on it while it moves: it stops there, as the grid
    // does (and a two-finger tap is a click of the right button).
    m_physics.stop();
    apply();
}

bool Scroller::eventFilter(QObject *watched, QEvent *event) {
    // Seen on its way into the window, and passed on untouched.
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        stopAt(static_cast<QMouseEvent *>(event)->scenePosition());
        break;
    case QEvent::TouchBegin:
        if (const auto *touch = static_cast<QTouchEvent *>(event); !touch->points().isEmpty())
            stopAt(touch->points().first().scenePosition());
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
