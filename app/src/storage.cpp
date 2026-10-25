#include "storage.h"

#include "portal.h"
#include "serviceclient.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QStorageInfo>

#include <cmath>

namespace {

QString withTilde(const QString &path) {
    const QString home = QDir::homePath();
    return path == home || path.startsWith(home + '/') ? '~' + path.mid(home.size()) : path;
}

// The disk a folder is on, found from its nearest existing ancestor (a new
// folder may not be there yet).
QStorageInfo diskOf(QString path) {
    while (!QFileInfo::exists(path) && path != QStringLiteral("/"))
        path = QFileInfo(path).absolutePath();
    return QStorageInfo(path);
}

} // namespace

Storage::Storage(ServiceClient *service, QObject *parent) : QObject(parent), m_service(service) {
    m_poll.setInterval(1000);
    connect(&m_poll, &QTimer::timeout, this, &Storage::refresh);
}

QString Storage::location() const { return withTilde(m_location); }
QString Storage::library() const { return withTilde(m_library); }

void Storage::setWatching(bool watching) {
    if (watching == m_watching)
        return;
    m_watching = watching;
    emit watchingChanged();
    if (watching) {
        m_error.clear();
        refresh();
    } else {
        m_poll.stop();
    }
}

void Storage::refresh() {
    m_service->call(QStringLiteral("storage"), {}, [this](bool ok, const QJsonObject &result, const QString &code) {
        if (ok)
            take(result);
        else
            failed(code, {});
    });
}

void Storage::setLimit(double bytes) {
    m_error.clear();
    m_service->call(QStringLiteral("set_storage"), {{"cap_bytes", qint64(bytes)}},
                    [this](bool ok, const QJsonObject &result, const QString &code) {
                        if (ok)
                            take(result);
                        else
                            failed(code, {});
                    });
    m_followUntil = QDateTime::currentMSecsSinceEpoch() + 4000;
    m_poll.start();
}

void Storage::chooseLocation() {
    auto *picker = new PortalFolderPicker(this);
    connect(picker, &PortalFolderPicker::picked, this, [this, picker](const QString &folder) {
        picker->deleteLater();
        if (!folder.isEmpty())
            moveTo(folder);
    });
    picker->pick(tr("Where to keep photos"));
}

void Storage::moveTo(const QString &chosen) {
    // Into a folder of its own, unless the one chosen is empty or is already
    // called pinhole: never loose among someone's other files.
    QString folder = QDir::cleanPath(chosen);
    const QDir dir(folder);
    if (!dir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden) &&
        QFileInfo(folder).fileName().compare(QStringLiteral("pinhole"), Qt::CaseInsensitive) != 0)
        folder += QStringLiteral("/pinhole");
    // On another disk the files are copied: there must be room for them.
    const QStorageInfo there = diskOf(folder);
    if (there.device() != diskOf(m_location).device() && double(there.bytesAvailable()) < m_used) {
        m_error = tr("Not enough room there: %1 needed, %2 free.").arg(size(m_used), size(double(there.bytesAvailable())));
        emit changed();
        return;
    }
    m_error.clear();
    m_service->call(QStringLiteral("set_storage"), {{"cache", folder}},
                    [this](bool ok, const QJsonObject &result, const QString &code) {
                        if (ok)
                            take(result);
                        else
                            failed(code, {});
                    });
    m_followUntil = QDateTime::currentMSecsSinceEpoch() + 4000;
    m_poll.start();
}

void Storage::take(const QJsonObject &report) {
    m_loaded = true;
    m_outdated = false;
    m_location = report.value("cache").toString();
    m_limit = report.value("cap_bytes").toDouble();
    m_minLimit = report.value("min_cap_bytes").toDouble();
    m_used = report.value("used_bytes").toDouble();
    m_kinds = report.value("kinds").toObject().toVariantMap();
    m_library = report.value("library").toString();
    m_libraryBytes = report.value("library_bytes").toDouble();
    m_freeing = report.value("freeing").toBool();
    const QJsonObject moving = report.value("moving").toObject();
    m_moving = !moving.isEmpty();
    m_moved = moving.value("done_bytes").toDouble();
    m_moveTotal = moving.value("total_bytes").toDouble();
    m_free = double(diskOf(m_location).bytesAvailable());
    // Followed while something is under way and the panel is open.
    if (m_watching && (m_freeing || m_moving || QDateTime::currentMSecsSinceEpoch() < m_followUntil))
        m_poll.start();
    else
        m_poll.stop();
    emit changed();
}

void Storage::failed(const QString &code, const QString &) {
    // A service from before settings existed answers bad_request to them.
    m_outdated = code == QLatin1String("bad_request") && !m_loaded;
    m_error = m_outdated
                  ? tr("The Pinhole service running now is older than this window and has to be restarted.")
              : code == QLatin1String("bad_request")
                  ? tr("That folder cannot be used.")
                  : tr("The Pinhole service is not answering.");
    m_poll.stop();
    emit changed();
}

void Storage::restartService() {
    // Only the service: a download under way starts again when asked for.
    QProcess::execute(QStringLiteral("pkill"), {QStringLiteral("-x"), QStringLiteral("pinholed")});
    m_error.clear();
    emit changed();
    QTimer::singleShot(1500, this, &Storage::refresh);
}

QString Storage::size(double bytes) const {
    // A decimal from 1 GB to 10 GB, unless the size is whole (the limits
    // are); none in MB, where it is noise.
    const double gb = bytes / double(1 << 30);
    const int decimals = gb >= 1 && gb < 10 && gb != std::floor(gb) ? 1 : 0;
    return QLocale().formattedDataSize(qint64(bytes), decimals, QLocale::DataSizeTraditionalFormat);
}
