#include "shield.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QWheelEvent>

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

void Shield::wheelEvent(QWheelEvent *event) { event->accept(); }

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
