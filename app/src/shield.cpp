#include "shield.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QWheelEvent>

#include <cmath>

namespace {
// `import Pinhole` has it wherever Pinhole's QML loads: the window, and the
// tests, which load its controls (ShellButton.qml stands on one) without it.
void registerShield() { qmlRegisterType<Shield>("Pinhole", 1, 0, "Shield"); }
} // namespace
Q_COREAPP_STARTUP_FUNCTION(registerShield)

Shield::Shield(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
}

void Shield::mousePressEvent(QMouseEvent *event) {
    m_pressed = event->button() == Qt::LeftButton;
    event->accept();
}

void Shield::mouseMoveEvent(QMouseEvent *event) { event->accept(); }

void Shield::mouseReleaseEvent(QMouseEvent *event) {
    event->accept();
    const bool click = m_pressed && event->button() == Qt::LeftButton && contains(event->position());
    m_pressed = false;
    if (click)
        emit clicked();
}

void Shield::mouseDoubleClickEvent(QMouseEvent *event) { event->accept(); }

void Shield::mouseUngrabEvent() { m_pressed = false; }

void Shield::setSteps(int steps) {
    if (steps == m_steps)
        return;
    m_steps = steps;
    emit stepsChanged();
}

void Shield::wheelEvent(QWheelEvent *event) {
    event->accept();
    if (m_steps == 0 || (event->modifiers() & Qt::ControlModifier))
        return;
    const bool across = m_steps & Qt::Horizontal, down = m_steps & Qt::Vertical;
    // Along the way it steps; both, whichever way it mostly goes.
    const auto along = [&](QPointF d) {
        if (across && down)
            return std::abs(d.x()) > std::abs(d.y()) ? d.x() : d.y();
        return across ? d.x() : d.y();
    };
    const QPoint angle = event->angleDelta(), pixels = event->pixelDelta();
    if (event->phase() == Qt::NoScrollPhase && !angle.isNull()) {
        // A notch a step: the wheel down, or to the right, the next.
        m_turned -= along(angle);
        for (; m_turned >= 120; m_turned -= 120)
            emit stepped(1);
        for (; m_turned <= -120; m_turned += 120)
            emit stepped(-1);
        return;
    }
    // A swipe: one step once it has gone far enough, then none until the
    // fingers lift, or rest a quarter of a second.
    const quint64 t = event->timestamp();
    if (event->phase() == Qt::ScrollBegin || t - m_lastSwipe > 250) {
        m_swiped = 0;
        m_swipeStepped = false;
    }
    m_lastSwipe = t;
    if (m_swipeStepped)
        return;
    m_swiped += along(pixels);
    if (std::abs(m_swiped) > 80) {
        m_swipeStepped = true;
        emit stepped(m_swiped > 0 ? -1 : 1);
    }
}

void Shield::hoverEnterEvent(QHoverEvent *event) { event->accept(); }

void Shield::hoverMoveEvent(QHoverEvent *event) { event->accept(); }

bool Shield::event(QEvent *event) {
    // A pinch or a swipe of three fingers, as the compositor passes it on.
    if (event->type() == QEvent::NativeGesture) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}
