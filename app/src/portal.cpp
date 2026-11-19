#include "portal.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QUrl>

PortalFolderPicker::PortalFolderPicker(QObject *parent) : QObject(parent) {}

void PortalFolderPicker::pick(const QString &title) {
    m_title = title;
    QDBusConnection bus = QDBusConnection::sessionBus();
    // The portal answers on a request object whose path follows from our
    // bus name and a token we choose; listening there before asking means
    // the answer cannot be missed.
    const QString token = QStringLiteral("pinhole%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    const QString handle = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), handle, QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"), this, SLOT(response(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"), QStringLiteral("OpenFile"));
    call << QString() << title
         << QVariantMap{{QStringLiteral("handle_token"), token},
                        {QStringLiteral("directory"), true},
                        {QStringLiteral("modal"), true},
                        {QStringLiteral("accept_label"), tr("Export")}};
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (QDBusPendingReply<QDBusObjectPath>(*w).isError()) {
            qWarning("pinhole: the desktop's folder picker is unavailable: Qt's instead");
            fallBack(m_title);
        }
    });
}

void PortalFolderPicker::response(uint code, const QVariantMap &results) {
    // 0 is success; 1 cancelled; 2 anything else, which is no answer: Qt's
    // own picker then, never nothing as though cancelled.
    if (code == 2) {
        qWarning("pinhole: the desktop's folder picker failed: Qt's instead");
        return fallBack(m_title);
    }
    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    emit picked(code == 0 && !uris.isEmpty() ? QUrl(uris.first()).toLocalFile() : QString());
}

void PortalFolderPicker::fallBack(const QString &title) {
    m_title = title;
    // Qt Quick's own folder dialog, drawn in the window, on a desktop with
    // no working FileChooser portal.
    QQuickWindow *window = nullptr;
    for (QWindow *each : QGuiApplication::topLevelWindows())
        if (auto *quick = qobject_cast<QQuickWindow *>(each); quick && qmlEngine(quick))
            window = window && window->isVisible() ? window : quick;
    if (!window) {
        emit picked({});
        return;
    }
    QQmlComponent component(qmlEngine(window));
    component.setData("import QtQuick.Dialogs\nFolderDialog { options: FolderDialog.DontUseNativeDialog }\n", QUrl());
    QObject *dialog = component.createWithInitialProperties(
        {{QStringLiteral("title"), m_title},
         {QStringLiteral("parentWindow"), QVariant::fromValue<QWindow *>(window)},
         {QStringLiteral("currentFolder"), QUrl::fromLocalFile(QDir::homePath())}});
    if (!dialog) {
        qWarning("pinhole: no folder picker: %s", qPrintable(component.errorString()));
        emit picked({});
        return;
    }
    dialog->setParent(this);
    connect(dialog, SIGNAL(accepted()), this, SLOT(chosen()));
    connect(dialog, SIGNAL(rejected()), this, SLOT(dismissed()));
    QMetaObject::invokeMethod(dialog, "open");
    m_dialog = dialog;
}

void PortalFolderPicker::chosen() {
    emit picked(m_dialog ? m_dialog->property("selectedFolder").toUrl().toLocalFile() : QString());
}

void PortalFolderPicker::dismissed() { emit picked({}); }
