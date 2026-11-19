#include "adjustment.h"

#include <QColorSpace>
#include <QStringList>
#include <QTransform>
#include <QUrlQuery>
#include <QVariantList>

#include <algorithm>
#include <cmath>

QSize Adjustment::applied(QSize size) const {
    if (turn % 2)
        size.transpose();
    const int w = std::max(1, int(std::lround(size.width() * crop.width())));
    const int h = std::max(1, int(std::lround(size.height() * crop.height())));
    return QSize(w, h);
}

QImage Adjustment::applied(const QImage &image) const {
    if (image.isNull() || isIdentity())
        return image;
    QImage turned = turn ? image.transformed(QTransform().rotate(90.0 * turn)) : image;
    if (crop == QRectF(0, 0, 1, 1))
        return turned;
    const QRect part(int(std::floor(crop.x() * turned.width())), int(std::floor(crop.y() * turned.height())),
                     std::max(1, int(std::lround(crop.width() * turned.width()))),
                     std::max(1, int(std::lround(crop.height() * turned.height()))));
    QImage cut = turned.copy(part.intersected(turned.rect()));
    cut.setColorSpace(image.colorSpace());
    return cut;
}

QString Adjustment::query() const {
    if (isIdentity())
        return {};
    return QStringLiteral("%1,%2,%3,%4,%5")
        .arg(turn)
        .arg(crop.x(), 0, 'g', 6)
        .arg(crop.y(), 0, 'g', 6)
        .arg(crop.width(), 0, 'g', 6)
        .arg(crop.height(), 0, 'g', 6);
}

Adjustment Adjustment::fromQuery(const QString &value) {
    const QStringList parts = value.split(',');
    if (parts.size() != 5)
        return {};
    Adjustment a;
    a.turn = parts[0].toInt();
    a.crop = QRectF(parts[1].toDouble(), parts[2].toDouble(), parts[3].toDouble(), parts[4].toDouble());
    return a.normalized();
}

QUrl Adjustment::withAdjustment(const QUrl &url, const Adjustment &adjustment) {
    if (adjustment.isIdentity() || url.isEmpty())
        return url;
    // An image provider's id is everything after its name: the query goes
    // there too, and comes back in requestImage's id.
    return QUrl(url.toString() + QStringLiteral("?adjust=") + adjustment.query());
}

Adjustment Adjustment::ofUrl(const QString &id, QString *bare) {
    const qsizetype at = id.lastIndexOf(QLatin1String("?adjust="));
    if (at < 0) {
        if (bare)
            *bare = id;
        return {};
    }
    if (bare)
        *bare = id.left(at);
    return fromQuery(id.mid(at + 8));
}

QVariantMap Adjustment::toMap() const {
    return {{"turn", turn}, {"crop", QVariantList{crop.x(), crop.y(), crop.width(), crop.height()}}};
}

Adjustment Adjustment::fromMap(const QVariantMap &map) {
    Adjustment a;
    a.turn = map.value("turn").toInt();
    const QVariantList crop = map.value("crop").toList();
    if (crop.size() == 4)
        a.crop = QRectF(crop[0].toDouble(), crop[1].toDouble(), crop[2].toDouble(), crop[3].toDouble());
    return a.normalized();
}

Adjustment Adjustment::normalized() const {
    Adjustment a;
    a.turn = ((turn % 4) + 4) % 4;
    const double x = std::clamp(crop.x(), 0.0, 1.0), y = std::clamp(crop.y(), 0.0, 1.0);
    const double w = std::clamp(crop.width(), 0.0, 1.0 - x), h = std::clamp(crop.height(), 0.0, 1.0 - y);
    // Nothing left, or not a number: all of it.
    a.crop = w > 0.01 && h > 0.01 ? QRectF(x, y, w, h) : QRectF(0, 0, 1, 1);
    // Within a hair of all of it: all of it, so it counts as untouched.
    if (std::abs(a.crop.x()) < 1e-4 && std::abs(a.crop.y()) < 1e-4 && std::abs(a.crop.width() - 1) < 1e-4
        && std::abs(a.crop.height() - 1) < 1e-4)
        a.crop = QRectF(0, 0, 1, 1);
    return a;
}

Adjustment Adjustment::turned(int quarters) const {
    Adjustment a = *this;
    for (int q = ((quarters % 4) + 4) % 4; q > 0; --q) {
        // A quarter clockwise: what was at the left is at the top.
        const QRectF c = a.crop;
        a.crop = QRectF(1 - c.y() - c.height(), c.x(), c.height(), c.width());
        a.turn = (a.turn + 1) % 4;
    }
    return a.normalized();
}

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

AdjustmentStore::AdjustmentStore(QObject *parent) : QObject(parent) {
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { read(); });
    // Written by rename: the file watched is gone, its folder says so.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (!m_file.isEmpty() && QFileInfo::exists(m_file) && !m_watcher.files().contains(m_file))
            m_watcher.addPath(m_file);
        read();
    });
}

QString AdjustmentStore::fileFor(const QString &libraryDir) {
    // As a plugin's cacheDir names a library: library/icloud/<apple id>
    // is icloud-<apple id>.
    const QDir library(libraryDir);
    QDir kind(library);
    kind.cdUp();
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/pinhole/adjustments/"
           + kind.dirName() + '-' + library.dirName() + ".json";
}

void AdjustmentStore::setLibrary(const QString &dir) {
    const QString file = dir.isEmpty() ? QString() : fileFor(dir);
    if (file == m_file)
        return;
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    m_file = file;
    if (!m_file.isEmpty()) {
        QDir().mkpath(QFileInfo(m_file).path());
        m_watcher.addPath(QFileInfo(m_file).path());
        if (QFileInfo::exists(m_file))
            m_watcher.addPath(m_file);
    }
    // Another library's edits are never this one's: none known until its
    // file is read (an unreadable one stays none, and is not written).
    bool had = false;
    {
        std::lock_guard lock(m_mutex);
        had = !m_all.isEmpty();
        m_all.clear();
    }
    if (had)
        emit changed();
    read();
}

namespace {
// False when the file is there but could not be read as edits: what it
// holds must not be written over as though it were none.
bool readFile(const QString &path, Adjustments *all) {
    all->clear();
    if (path.isEmpty() || !QFileInfo::exists(path))
        return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject kept = document.object();
    for (auto it = kept.begin(); it != kept.end(); ++it) {
        const Adjustment a = Adjustment::fromMap(it.value().toObject().toVariantMap());
        if (!a.isIdentity())
            all->insert(it.key().toUtf8(), a);
    }
    return true;
}
} // namespace

Adjustments AdjustmentStore::load(const QString &libraryDir) {
    Adjustments all;
    if (!libraryDir.isEmpty())
        readFile(fileFor(libraryDir), &all);
    return all;
}

void AdjustmentStore::read() {
    Adjustments all;
    // Half written by another (never, as it is renamed into place) or
    // damaged: what was read before stays.
    if (!readFile(m_file, &all))
        return;
    {
        std::lock_guard lock(m_mutex);
        if (all == m_all)
            return;
        m_all = all;
    }
    emit changed();
}

Adjustments AdjustmentStore::snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_all;
}

Adjustment AdjustmentStore::of(const QByteArray &id) const {
    std::lock_guard lock(m_mutex);
    return m_all.value(id);
}

bool AdjustmentStore::set(const Adjustments &changes) {
    if (m_file.isEmpty())
        return false;
    // Another window may have written its own since this one read the
    // file: under a lock they share, the file read afresh and only these
    // changed in it, so neither's crop is lost to the other's.
    const int lock = ::open(QFile::encodeName(m_file + QStringLiteral(".lock")).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0)
        return false;
    const auto unlock = qScopeGuard([lock] { ::close(lock); });
    if (::flock(lock, LOCK_EX) != 0)
        return false;
    Adjustments all;
    if (!readFile(m_file, &all))
        return false;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const Adjustment a = it.value().normalized();
        if (a.isIdentity())
            all.remove(it.key());
        else
            all.insert(it.key(), a);
    }
    QJsonObject kept;
    for (auto it = all.begin(); it != all.end(); ++it)
        kept.insert(QString::fromUtf8(it.key()), QJsonObject::fromVariantMap(it.value().toMap()));
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(kept).toJson(QJsonDocument::Compact)) < 0 || !file.commit())
        return false;
    {
        std::lock_guard lock(m_mutex);
        if (all == m_all)
            return true;
        m_all = all;
    }
    emit changed();
    return true;
}
