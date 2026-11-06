#include "scroller.h"

#include "touchpadhold.h"
#include "zoomer.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QNativeGestureEvent>
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
// `import Pinhole` has them wherever Pinhole's QML loads (Shield's alike).
void registerScroller() {
    qmlRegisterType<Scroller>("Pinhole", 1, 0, "Scroller");
}
} // namespace
Q_COREAPP_STARTUP_FUNCTION(registerScroller)

void Scroller::setTouchpadHold(TouchpadHold *hold) { theHold = hold; }
TouchpadHold *Scroller::touchpadHold() { return theHold; }

Scroller::Scroller(QQuickItem *parent) : QQuickItem(parent) {
    // The wheel only: presses and touches go on to what is beneath.
    setAcceptedMouseButtons(Qt::NoButton);
    if (theHold) {
        connect(theHold, &TouchpadHold::began, this, [this](quint32 time) {
            if (isVisible() && moving()) {
                m_y.physics.touch(double(time));
                m_x.physics.touch(double(time));
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
    m_y.physics.stop();
    m_x.physics.stop();
    if (isComponentComplete())
        adopt();
    emit flickableChanged();
}

void Scroller::componentComplete() {
    QQuickItem::componentComplete();
    // Declared inside a Flickable: that one, and over it rather than in
    // what it scrolls.
    if (!m_flickable)
        for (QQuickItem *up = parentItem(); up; up = up->parentItem())
            if (up->inherits("QQuickFlickable")) {
                m_flickable = up;
                emit flickableChanged();
                break;
            }
    adopt();
}

void Scroller::adopt() {
    for (const auto &c : std::as_const(m_fitting))
        disconnect(c);
    m_fitting.clear();
    if (!m_flickable)
        return;
    QQuickItem *content = m_flickable->property("contentItem").value<QQuickItem *>();
    // Declared in it (in what it scrolls, or its own child): over it.
    if (parentItem() == m_flickable || (content && parentItem() == content)) {
        setParentItem(m_flickable);
        // Above what it shows, its delegates and their handlers too.
        setZ(1e6);
        m_fitting << connect(m_flickable, &QQuickItem::widthChanged, this, &Scroller::fit)
                  << connect(m_flickable, &QQuickItem::heightChanged, this, &Scroller::fit);
        fit();
    }
}

void Scroller::fit() {
    if (m_flickable && parentItem() == m_flickable) {
        setPosition({0, 0});
        setSize(m_flickable->size());
    }
}

double Scroller::at(const Axis &axis) const { return m_flickable ? m_flickable->property(axis.position).toDouble() : 0; }

double Scroller::view(const Axis &axis) const {
    return m_flickable ? (axis.across ? m_flickable->width() : m_flickable->height()) : 0;
}

void Scroller::syncBounds(Axis &axis) {
    if (!m_flickable)
        return;
    const double origin = m_flickable->property(axis.origin).toDouble();
    const double low = origin - m_flickable->property(axis.before).toDouble();
    const double high = origin + m_flickable->property(axis.content).toDouble()
                        + m_flickable->property(axis.after).toDouble() - view(axis);
    axis.low = low;
    axis.high = std::max(low, high);
    // Pulled past an end it gives as the grid does, for the view's length.
    axis.physics.setBounds(axis.low, axis.high, view(axis));
}

bool Scroller::scrolls(const Axis &axis) const {
    if (!m_flickable)
        return false;
    // Content no larger than the view goes nowhere that way.
    return m_flickable->property(axis.content).toDouble() + m_flickable->property(axis.before).toDouble()
               + m_flickable->property(axis.after).toDouble()
           > view(axis) + 0.5;
}

void Scroller::pickUp(Axis &axis) {
    const double p = at(axis);
    if ((!axis.physics.animating() && !axis.physics.tracking()) || std::abs(p - axis.written) > 0.5)
        axis.physics.jumpTo(p);
    axis.written = p;
}

bool Scroller::moving() const {
    return m_y.physics.animating() || m_x.physics.animating() || m_y.physics.tracking() || m_x.physics.tracking();
}

void Scroller::apply() {
    if (!m_flickable)
        return;
    for (Axis *axis : {&m_y, &m_x}) {
        // Only a way it goes: a list's contentX is the ListView's own.
        if (!scrolls(*axis) && !axis->physics.animating())
            continue;
        axis->written = axis->physics.position();
        m_flickable->setProperty(axis->position, axis->written);
    }
    if ((m_y.physics.animating() || m_x.physics.animating()) && !m_ticking && m_window) {
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
    const double dt = std::clamp(m_frameClock.restart() / 1000.0, 0.0, 0.05);
    m_ticking = advance(dt);
    if (m_ticking && m_window)
        m_window->update();
}

bool Scroller::advance(double dt) {
    bool still = false;
    for (Axis *axis : {&m_y, &m_x}) {
        if (!axis->physics.animating())
            continue;
        // Moved by something else meanwhile (keys, a drag, the list growing
        // under it): that is where it is now.
        if (!m_flickable || std::abs(at(*axis) - axis->written) > 0.5) {
            axis->physics.jumpTo(at(*axis));
            axis->written = at(*axis);
            continue;
        }
        syncBounds(*axis);
        still = axis->physics.step(dt) || still;
        axis->written = axis->physics.position();
        m_flickable->setProperty(axis->position, axis->written);
    }
    return still;
}

bool Scroller::canMove(bool across, double d) const {
    const Axis &axis = across ? m_x : m_y;
    if (!m_flickable || d == 0 || !scrolls(axis))
        return false;
    const double p = at(axis);
    const double origin = m_flickable->property(axis.origin).toDouble();
    const double low = origin - m_flickable->property(axis.before).toDouble();
    const double high = std::max(low, origin + m_flickable->property(axis.content).toDouble()
                                          + m_flickable->property(axis.after).toDouble() - view(axis));
    // Up the wheel or the fingers, up the content: towards its top (its
    // left).
    return d > 0 ? p > low + 0.5 : p < high - 0.5;
}

Scroller *Scroller::innerAt(QPointF scenePosition, bool across, double d) const {
    QQuickItem *content = m_flickable ? m_flickable->property("contentItem").value<QQuickItem *>() : nullptr;
    if (!content || d == 0)
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
                if (inner != this && inner->contains(inner->mapFromScene(scenePosition))
                    && (inner->canMove(across, d) || inner->innerAt(scenePosition, across, d)))
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

Zoomer *Scroller::zoomer() const {
    for (QQuickItem *up = parentItem(); up; up = up->parentItem())
        if (auto *zoom = qobject_cast<Zoomer *>(up))
            return zoom->isEnabled() ? zoom : nullptr;
    return nullptr;
}

void Scroller::wheelEvent(QWheelEvent *event) {
    // Every one is its own, whatever it does here: none goes on beneath.
    event->accept();
    take(event);
}

void Scroller::take(const QWheelEvent *event) {
    if (!m_flickable)
        return;
    const QPoint pixels = event->pixelDelta(), angle = event->angleDelta();
    // Ctrl and the wheel zoom, as on the grid: the PinchZoom it is in, if
    // any. So does the sideways wheel where nothing here goes across.
    const bool sidewaysNotch = event->phase() == Qt::NoScrollPhase && std::abs(angle.x()) > std::abs(angle.y());
    if ((event->modifiers() & Qt::ControlModifier) || (sidewaysNotch && !scrolls(m_x))) {
        if (Zoomer *zoom = zoomer())
            zoom->takeWheel(event);
        return;
    }
    const double t = double(event->timestamp());
    const double dy = !pixels.isNull() ? pixels.y() : angle.y() / 8.0;
    const double dx = !pixels.isNull() ? pixels.x() : angle.x() / 8.0;
    // Which way it mostly goes, for who has it: a notch over what only
    // goes across, across.
    const bool notchAcross = event->phase() == Qt::NoScrollPhase && !angle.isNull() && !scrolls(m_y) && scrolls(m_x);
    const bool across = std::abs(dx) > std::abs(dy) || notchAcross;
    const double d = std::abs(dx) > std::abs(dy) ? dx : dy;
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
        if (!m_swipeDecided && (dx != 0 || dy != 0)) {
            m_swipeDecided = true;
            if ((m_inner = innerAt(event->scenePosition(), across, d))) {
                // Its, from the fingers landing; this one's still where
                // they landed.
                m_y.physics.end(t);
                m_x.physics.end(t);
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
        if (Scroller *inner = innerAt(event->scenePosition(), across, d)) {
            inner->take(event);
            return;
        }
        break;
    }
    for (Axis *axis : {&m_y, &m_x}) {
        pickUp(*axis);
        syncBounds(*axis);
    }
    // As the grid takes them (PhotoGrid::wheelEvent): a touchpad's begin,
    // updates and end, and the momentum a Mac's adds; a wheel's notches
    // glide; pixels without phases are followed as they come. Each way the
    // content goes, its own.
    switch (event->phase()) {
    case Qt::ScrollBegin:
        for (Axis *axis : {&m_y, &m_x})
            if (scrolls(*axis) && !axis->physics.tracking())
                axis->physics.begin(t);
        break;
    case Qt::ScrollUpdate:
    case Qt::ScrollMomentum:
        if (scrolls(m_y))
            m_y.physics.move(t, -dy);
        if (scrolls(m_x))
            m_x.physics.move(t, -dx);
        break;
    case Qt::ScrollEnd:
        m_y.physics.end(t);
        m_x.physics.end(t);
        break;
    case Qt::NoScrollPhase:
        if (angle.isNull() && !pixels.isNull()) {
            // Smooth scrolling without phases or notches: followed directly.
            for (Axis *axis : {&m_y, &m_x}) {
                const double by = axis->across ? pixels.x() : pixels.y();
                if (scrolls(*axis) && by != 0)
                    axis->physics.jumpTo(std::clamp(axis->physics.position() - by, axis->low, axis->high));
            }
        } else {
            // A notch: glided, and sped up the longer it spins
            // (ScrollPhysics::wheel()); over what only goes across, the
            // wheel goes across.
            Axis &axis = across ? m_x : m_y;
            if (scrolls(axis))
                axis.physics.wheel(t, -(notchAcross ? dy : d) * kGridWheel);
        }
        break;
    }
    apply();
}

bool Scroller::event(QEvent *event) {
    // A pinch zooms the PinchZoom it is in, if any; nothing else, the
    // photos beneath included.
    if (event->type() == QEvent::NativeGesture) {
        if (Zoomer *zoom = zoomer())
            zoom->takeGesture(static_cast<QNativeGestureEvent *>(event));
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

bool Scroller::stopAt(QPointF scenePosition) {
    if (!isVisible() || !isEnabled() || !moving())
        return false;
    if (!contains(mapFromScene(scenePosition)))
        return false;
    // Moved by something else meanwhile (a drag, its keys): not this one's
    // to stop.
    if (!m_flickable)
        return false;
    for (const Axis *axis : {&m_y, &m_x})
        if ((axis->physics.animating() || axis->physics.tracking()) && std::abs(at(*axis) - axis->written) > 0.5)
            return false;
    // A click or a touch on it while it moves: it stops there, as the grid
    // does (and a two-finger tap is a click of the right button).
    m_y.physics.stop();
    m_x.physics.stop();
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
