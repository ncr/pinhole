#include "control.h"

#include <QAccessible>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QQmlEngine>
#include <QtQuick/private/qaccessiblequickitem_p.h>
#include <QtQuick/private/qquickitem_p.h>

namespace {
class AccessibleControl final : public QAccessibleQuickItem {
public:
    explicit AccessibleControl(ControlSurface *item) : QAccessibleQuickItem(item), m_item(item) {}
    QAccessible::State state() const override {
        auto result = QAccessibleQuickItem::state();
        result.disabled = result.disabled || !m_item->usable();
        if (result.disabled) result.focusable = false;
        return result;
    }
    void doAction(const QString &name) override {
        if (m_item->usable() && m_item->isEnabled()) QAccessibleQuickItem::doAction(name);
    }
private:
    ControlSurface *m_item;
};

QAccessibleInterface *accessibleControl(const QString &name, QObject *object) {
    if (name == QLatin1String("ControlSurface"))
        if (auto *item = qobject_cast<ControlSurface *>(object)) return new AccessibleControl(item);
    return nullptr;
}
void registerControl() {
    QAccessible::installFactory(accessibleControl);
    // Pinhole 1.0 inherits QtQuick Item properties added after QtQuick 2.0
    // (notably activeFocusOnTab); expose the complete Qt 5 Item revision.
    qmlRegisterRevision<QQuickItem, 15>("Pinhole", 1, 0);
    qmlRegisterType<ControlSurface>("Pinhole", 1, 0, "ControlSurface");
    qmlRegisterUncreatableType<FocusFence>("Pinhole", 1, 0, "FocusFence", "Use the attached enabled property on a modal item");
}
bool activation(int key) { return key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter; }
}
Q_COREAPP_STARTUP_FUNCTION(registerControl)

FocusFence::FocusFence(QObject *parent) : QObject(parent) {}
void FocusFence::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (auto *item = qobject_cast<QQuickItem *>(parent())) QQuickItemPrivate::get(item)->isTabFence = enabled;
    emit enabledChanged();
}

ControlSurface::ControlSurface(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton); // QML handlers own pointer presses.
    setAcceptHoverEvents(true);
    connect(this, &QQuickItem::enabledChanged, this, [this] { if (!isEnabled()) releaseKey(); });
}
void ControlSurface::setUsable(bool usable) {
    if (m_usable == usable) return;
    m_usable = usable;
    if (!usable) { releaseKey(); if (hasActiveFocus()) setFocus(false); }
    emit usableChanged();
    QAccessible::State changed;
    changed.disabled = changed.focusable = true;
    QAccessibleStateChangeEvent event(this, changed);
    QAccessible::updateAccessibility(&event);
}
bool ControlSurface::event(QEvent *event) {
    if (event->type() == QEvent::ShortcutOverride && hasActiveFocus() && m_usable && isEnabled()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (activation(key->key()) && key->modifiers() == Qt::NoModifier) { event->accept(); return true; }
    }
    return QQuickItem::event(event);
}
void ControlSurface::keyPressEvent(QKeyEvent *event) {
    if (m_usable && activation(event->key()) && event->modifiers() == Qt::NoModifier) {
        if (!event->isAutoRepeat() && !m_key) { m_key = event->key(); emit keyboardPressedChanged(); }
        event->accept();
    } else QQuickItem::keyPressEvent(event);
}
void ControlSurface::keyReleaseEvent(QKeyEvent *event) {
    if (activation(event->key())) {
        const bool fire = m_key == event->key() && !event->isAutoRepeat() && m_usable && isEnabled();
        if (!event->isAutoRepeat()) releaseKey();
        if (fire) emit activated();
        event->accept();
    } else QQuickItem::keyReleaseEvent(event);
}
void ControlSurface::focusOutEvent(QFocusEvent *event) { releaseKey(); QQuickItem::focusOutEvent(event); }
void ControlSurface::releaseKey() { if (m_key) { m_key = 0; emit keyboardPressedChanged(); } }
