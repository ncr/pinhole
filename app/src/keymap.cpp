#include "keymap.h"

#include <QCoreApplication>
#include <QKeySequence>
#include <QMap>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <private/qqmldata_p.h>

Keymap::Keymap(QObject *parent) : QObject(parent) {
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installEventFilter(this);
}

void Keymap::setLayer(const QString &layer) {
    if (layer == m_layer)
        return;
    m_layer = layer;
    emit layerChanged();
}

void Keymap::setPlugin(const QString &plugin) {
    if (plugin == m_plugin)
        return;
    m_plugin = plugin;
    emit pluginChanged();
}

void Keymap::setInput(const QString &input) {
    if (input == m_input)
        return;
    m_input = input;
    emit inputChanged();
}

void Keymap::add(QObject *action) {
    if (!action || m_actions.contains(action))
        return;
    // Where it is written, for the help to keep that order: bindings are
    // evaluated in no order to go by.
    if (const QQmlContext *context = qmlContext(action))
        action->setProperty("file", context->baseUrl().toString());
    if (const QQmlData *data = QQmlData::get(action))
        action->setProperty("line", int(data->lineNumber));
    m_actions.append(action);
    ++m_revision;
    emit actionsChanged();
}

void Keymap::remove(QObject *action) {
    if (m_actions.removeAll(action) == 0)
        return;
    ++m_revision;
    emit actionsChanged();
}

QVariantList Keymap::actions() const {
    QVariantList list;
    for (const QPointer<QObject> &action : m_actions)
        if (action)
            list.append(QVariant::fromValue(action.data()));
    return list;
}

QStringList Keymap::clashes() const {
    QMap<QString, QStringList> byKey;
    for (const QPointer<QObject> &action : m_actions) {
        if (!action || !action->property("live").toBool())
            continue;
        const QString name = action->property("name").toString();
        for (const QVariant &key : action->property("keys").toList()) {
            const QString portable = QKeySequence(key.toString()).toString(QKeySequence::PortableText);
            if (!portable.isEmpty() && !byKey[portable].contains(name))
                byKey[portable].append(name);
        }
    }
    QStringList clashes;
    for (auto it = byKey.cbegin(); it != byKey.cend(); ++it)
        if (it.value().size() > 1)
            clashes.append(it.key() + QStringLiteral(": ") + it.value().join(QStringLiteral(", ")));
    return clashes;
}

QStringList Keymap::uncovered() const {
    QStringList missing;
    for (const QPointer<QObject> &action : m_actions) {
        if (!action)
            continue;
        const QString name = action->property("name").toString();
        const bool button = action->property("buttons").toInt() > 0;
        if (action->property("keys").toList().isEmpty() && action->property("noKeyboard").toString().isEmpty())
            missing.append(name + QStringLiteral(": keyboard"));
        if (!button && action->property("mouse").toString().isEmpty() && action->property("noMouse").toString().isEmpty())
            missing.append(name + QStringLiteral(": mouse"));
        if (!button && action->property("touchpad").toString().isEmpty()
            && action->property("noTouchpad").toString().isEmpty())
            missing.append(name + QStringLiteral(": touchpad"));
    }
    missing.removeDuplicates();
    return missing;
}

bool Keymap::lostField(QObject *watched, QEvent *event) {
    // What has the keys: the item a key is for, or the window's.
    auto *item = qobject_cast<QQuickItem *>(watched);
    if (auto *window = qobject_cast<QQuickWindow *>(watched))
        item = window->activeFocusItem();
    if (!item || !item->hasActiveFocus() || (item->isVisible() && item->isEnabled()))
        return false;
    QQuickWindow *window = item->window();
    if (!window || item == window->contentItem())
        return false;
    // Hidden or turned off with the keys, a field (a panel's, a plugin's)
    // would go on taking them, each of Pinhole's a shortcut it takes first,
    // until a restart: they go back to the photos.
    if (auto *grid = window->findChild<QQuickItem *>(QStringLiteral("grid")); grid && grid->isVisible())
        grid->forceActiveFocus();
    if (item->hasActiveFocus())
        item->setFocus(false);
    if (watched == window)
        return false;
    // Not this item's to take: a shortcut goes ahead, a key goes nowhere.
    event->ignore();
    return true;
}

bool Keymap::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    // A key that is a shortcut never arrives as a key press: Qt asks
    // whether anything wants it first (ShortcutOverride), then triggers it.
    case QEvent::KeyPress:
    case QEvent::ShortcutOverride:
        setInput(QStringLiteral("keyboard"));
        return lostField(watched, event);
    case QEvent::Shortcut:
        setInput(QStringLiteral("keyboard"));
        break;
    case QEvent::Wheel: {
        // A touchpad scrolls in phases (begin, update, end) and a mouse's
        // wheel without; libinput says which.
        const auto *wheel = static_cast<QWheelEvent *>(event);
        const bool touchpad = wheel->phase() != Qt::NoScrollPhase
                              || (wheel->pointingDevice() && wheel->pointingDevice()->type() == QInputDevice::DeviceType::TouchPad);
        m_pointer = touchpad ? QStringLiteral("touchpad") : QStringLiteral("mouse");
        setInput(m_pointer);
        break;
    }
    case QEvent::NativeGesture:
        m_pointer = QStringLiteral("touchpad");
        setInput(m_pointer);
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->pointingDevice() && mouse->pointingDevice()->type() == QInputDevice::DeviceType::TouchPad)
            m_pointer = QStringLiteral("touchpad");
        // Moving the pointer counts only once it is clearly in use, not
        // when a window opens under it.
        if (event->type() == QEvent::MouseButtonPress || m_input != QStringLiteral("keyboard"))
            setInput(m_pointer);
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
