#include "portal.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>
#include <QUrl>

PortalFolderPicker::PortalFolderPicker(QObject *parent) : QObject(parent) {}

void PortalFolderPicker::pick(const QString &title) {
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
            qWarning("pinhole: the folder picker is unavailable");
            emit picked({});
        }
    });
}

void PortalFolderPicker::response(uint code, const QVariantMap &results) {
    // 0 is success; 1 cancelled; 2 anything else.
    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    emit picked(code == 0 && !uris.isEmpty() ? QUrl(uris.first()).toLocalFile() : QString());
}
