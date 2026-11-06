#include "panzoom.h"

#include "keymap.h"
#include "scroller.h"
#include "touchpadhold.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QNativeGestureEvent>
#include <QQuickWindow>
#include <QStyleHints>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
// Endless: a map goes on, and its camera says where it ends.
constexpr double kFar = 1e12;
// A drag let go faster than this coasts on (px/s), measured over its last
// moves; one held still this long before it was let go does not.
constexpr double kThrowAbove = 300;
constexpr qint64 kRecentMs = 60;
constexpr qint64 kHeldMs = 70;
} // namespace

PanZoom::PanZoom(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    for (ScrollPhysics *axis : {&m_x, &m_y, &m_zoom})
        axis->setBounds(-kFar, kFar, 1000);
    m_clock.start();
    // Fingers coming to rest on the touchpad stop a coast, as on the grid.
    if (TouchpadHold *hold = Scroller::touchpadHold())
        connect(hold, &TouchpadHold::began, this, [this](quint32 time) {
            if (!isVisible() || !moving())
                return;
            m_x.touch(double(time));
            m_y.touch(double(time));
            m_zoom.stop();
            settle();
        });
}

PanZoom::~PanZoom() {
    if (m_window)
        m_window->removeEventFilter(this);
}

void PanZoom::setZoomPerNotch(qreal steps) {
    if (qFuzzyCompare(steps, m_zoomPerNotch) || steps <= 0)
        return;
    m_zoomPerNotch = steps;
    emit zoomPerNotchChanged();
}

bool PanZoom::moving() const {
    return m_dragging || m_x.animating() || m_y.animating() || m_zoom.animating() || m_x.tracking() || m_y.tracking();
}

void PanZoom::stop() {
    for (ScrollPhysics *axis : {&m_x, &m_y, &m_zoom})
        axis->stop();
    settle();
}

void PanZoom::zoomBy(qreal x, qreal y, qreal steps) {
    m_zoomAround = QPointF(x, y);
    m_zoom.nudge(steps / m_zoomPerNotch * kNotch, 0.25);
    run();
}

void PanZoom::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemSceneChange) {
        if (m_window) {
            m_window->removeEventFilter(this);
            disconnect(m_frames);
        }
        m_window = value.window;
        if (m_window) {
            m_window->installEventFilter(this);
            m_frames = connect(m_window, &QQuickWindow::afterAnimating, this, &PanZoom::tick);
        }
    }
    QQuickItem::itemChange(change, value);
}

bool PanZoom::eventFilter(QObject *watched, QEvent *event) {
    // A swipe's moves that carry pixels and no angle: Qt Quick drops them
    // before any item has them. Over it, each is given to the window again
    // with an angle, which Qt delivers as any wheel event: to it, or to what
    // is over it there (a panel of Pinhole's). Its pixels are what is read.
    static bool again = false;
    if (again || watched != m_window || event->type() != QEvent::Wheel || !isVisible() || !isEnabled())
        return false;
    const auto *wheel = static_cast<QWheelEvent *>(event);
    if (!wheel->angleDelta().isNull() || wheel->pixelDelta().isNull() || wheel->phase() == Qt::NoScrollPhase
        || !contains(mapFromScene(wheel->scenePosition())))
        return false;
    QWheelEvent copy(wheel->position(), wheel->globalPosition(), wheel->pixelDelta(), wheel->pixelDelta(), wheel->buttons(),
                     wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source(), wheel->pointingDevice());
    copy.setTimestamp(wheel->timestamp());
    again = true;
    QCoreApplication::sendEvent(m_window, &copy);
    again = false;
    event->accept();
    return true;
}

void PanZoom::wheelEvent(QWheelEvent *event) {
    // Over it or a child of it (a marker takes no wheel): its own, all.
    takeWheel(event, event->position());
    event->accept();
}

bool PanZoom::event(QEvent *event) {
    if (event->type() == QEvent::NativeGesture) {
        const auto *gesture = static_cast<QNativeGestureEvent *>(event);
        takeGesture(gesture, gesture->position());
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

void PanZoom::takeWheel(const QWheelEvent *event, QPointF at) {
    const QPoint pixels = event->pixelDelta(), angle = event->angleDelta();
    const double t = double(event->timestamp());
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    if (Keymap::isTouchpadScroll(*event) && !ctrl) {
        // A swipe pans, as the grid scrolls: begin, the fingers' moves (and
        // a Mac's momentum), end; pixels without phases, followed.
        const double dx = !pixels.isNull() ? pixels.x() : angle.x() / 8.0;
        const double dy = !pixels.isNull() ? pixels.y() : angle.y() / 8.0;
        switch (event->phase()) {
        case Qt::ScrollBegin:
            m_zoom.stop();
            if (!m_x.tracking())
                m_x.begin(t);
            if (!m_y.tracking())
                m_y.begin(t);
            break;
        case Qt::ScrollUpdate:
        case Qt::ScrollMomentum:
            m_x.move(t, -dx);
            m_y.move(t, -dy);
            break;
        case Qt::ScrollEnd:
            m_x.end(t);
            m_y.end(t);
            break;
        case Qt::NoScrollPhase:
            m_x.jumpTo(m_x.position() - dx);
            m_y.jumpTo(m_y.position() - dy);
            break;
        }
        run();
        return;
    }
    // The wheel (and Ctrl with a swipe) zooms around the pointer, gliding
    // notch by notch; the sideways wheel pans across.
    const double notches = angle.y() ? angle.y() / 120.0 : pixels.y() / 120.0;
    if (notches != 0) {
        m_zoomAround = at;
        m_zoom.wheel(t, notches * kNotch);
    } else if (angle.x() != 0) {
        m_x.wheel(t, -angle.x() / 8.0 * Scroller::kGridWheel);
    }
    run();
}

void PanZoom::takeGesture(const QNativeGestureEvent *event, QPointF at) {
    switch (event->gestureType()) {
    case Qt::BeginNativeGesture:
        // The fingers have it: what coasted stops.
        stop();
        break;
    case Qt::ZoomNativeGesture:
        // With the fingers, as they spread: no glide.
        if (const double factor = 1 + event->value(); factor > 0)
            emit zoomed(at.x(), at.y(), std::log2(factor));
        break;
    default:
        break;
    }
}

void PanZoom::run() {
    settle();
    if (!m_ticking && m_window && (m_x.animating() || m_y.animating() || m_zoom.animating())) {
        m_ticking = true;
        m_frameClock.restart();
        m_window->update();
    }
}

void PanZoom::tick() {
    if (!m_ticking)
        return;
    if (TouchpadHold *hold = Scroller::touchpadHold())
        hold->poll();
    const double dt = std::clamp(m_frameClock.restart() / 1000.0, 0.0, 0.05);
    m_ticking = advance(dt);
    if (m_ticking && m_window)
        m_window->update();
}

bool PanZoom::advance(double dt) {
    bool still = false;
    for (ScrollPhysics *axis : {&m_x, &m_y, &m_zoom})
        if (axis->animating())
            still = axis->step(dt) || still;
    settle();
    return still;
}

void PanZoom::settle() {
    // What moved since last said, said: the content goes the other way
    // from the physics' position, as a Scroller's content does.
    const double dx = m_x.position() - m_xAt, dy = m_y.position() - m_yAt, dz = m_zoom.position() - m_zoomAt;
    m_xAt = m_x.position();
    m_yAt = m_y.position();
    m_zoomAt = m_zoom.position();
    if (dx != 0 || dy != 0)
        emit panned(-dx, -dy);
    if (dz != 0)
        emit zoomed(m_zoomAround.x(), m_zoomAround.y(), dz / kNotch * m_zoomPerNotch);
    if (const bool now = moving(); now != m_wasMoving) {
        m_wasMoving = now;
        emit movingChanged();
    }
}

void PanZoom::mousePressEvent(QMouseEvent *event) {
    // A press while it moves only stops it, as on the grid.
    m_stoppedIt = moving();
    stop();
    m_pressed = true;
    m_pressAt = m_last = event->position();
    m_samples = {{m_clock.elapsed(), m_last}};
    event->accept();
}

void PanZoom::mouseMoveEvent(QMouseEvent *event) {
    if (!m_pressed)
        return;
    const QPointF at = event->position();
    if (!m_dragging) {
        if ((at - m_pressAt).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
            return;
        m_dragging = true;
        setKeepMouseGrab(true);
        emit draggingChanged();
        settle();
    }
    // One to one: the content under the pointer stays under it.
    const QPointF d = at - m_last;
    m_last = at;
    m_x.jumpTo(m_x.position() - d.x());
    m_y.jumpTo(m_y.position() - d.y());
    m_xAt = m_x.position();
    m_yAt = m_y.position();
    emit panned(d.x(), d.y());
    m_samples.push_back({m_clock.elapsed(), at});
    while (m_samples.size() > 2 && m_samples.back().t - m_samples.front().t > kRecentMs)
        m_samples.pop_front();
}

void PanZoom::mouseReleaseEvent(QMouseEvent *event) {
    const bool dragged = m_dragging;
    m_pressed = false;
    if (dragged) {
        m_dragging = false;
        setKeepMouseGrab(false);
        emit draggingChanged();
        // Let go moving: it coasts on as a swipe does.
        const qint64 now = m_clock.elapsed();
        if (m_samples.size() >= 2 && now - m_samples.back().t < kHeldMs) {
            const Sample &first = m_samples.front(), &last = m_samples.back();
            const double seconds = std::max<qint64>(1, last.t - first.t) / 1000.0;
            const QPointF velocity = (last.at - first.at) / seconds;
            if (std::hypot(velocity.x(), velocity.y()) > kThrowAbove) {
                m_x.fling(-velocity.x());
                m_y.fling(-velocity.y());
            }
        }
        run();
    } else if (!m_stoppedIt) {
        emit tapped(event->position().x(), event->position().y());
    }
    m_samples.clear();
    event->accept();
}

void PanZoom::mouseDoubleClickEvent(QMouseEvent *event) {
    emit doubleTapped(event->position().x(), event->position().y());
    event->accept();
}

void PanZoom::mouseUngrabEvent() {
    if (m_dragging) {
        m_dragging = false;
        emit draggingChanged();
    }
    m_pressed = false;
    m_samples.clear();
    settle();
}
