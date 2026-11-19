#include "synced.h"

#include "propertymap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <memory>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QString stateRoot() { return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/pinhole"; }

// The folder's lock (core/src/synced.rs, Lock), held while a file of it is
// read, merged into and written again.
class FolderLock {
public:
    explicit FolderLock(const QString &folder) {
        QDir().mkpath(folder);
        m_fd = ::open(QFile::encodeName(folder + "/.lock").constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (m_fd >= 0 && ::flock(m_fd, LOCK_EX) != 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }
    ~FolderLock() {
        if (m_fd >= 0) {
            ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
        }
    }
    bool held() const { return m_fd >= 0; }

private:
    int m_fd = -1;
};
} // namespace

SyncedStore::Seen SyncedStore::seen(const QString &path) {
    struct stat st{};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return {};
    return {quint64(st.st_ino), qint64(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec, qint64(st.st_size)};
}

SyncedStore::SyncedStore(QObject *parent) : QObject(parent), m_device(device()) {
    // Written by rename: the folder says so, for every file in it.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &SyncedStore::reread);
}

SyncedStore::~SyncedStore() { flush(); }

QString SyncedStore::folderFor(const QString &libraryDir) {
    // As a plugin's cacheDir names a library: library/icloud/<apple id> is
    // icloud-<apple id>.
    const QDir library(libraryDir);
    QDir kind(library);
    kind.cdUp();
    return stateRoot() + "/synced/" + kind.dirName() + '-' + library.dirName();
}

QString SyncedStore::fileIn(const QString &folder, const QString &plugin) { return folder + '/' + plugin + ".json"; }

QString SyncedStore::device() {
    const QString path = stateRoot() + "/device";
    const auto kept = [&path] {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
    };
    if (const QString id = kept(); !id.isEmpty())
        return id;
    QDir().mkpath(stateRoot());
    const QString id = QString::number(QRandomGenerator::system()->generate64() & 0xffffffffffffULL, 16).rightJustified(12, '0');
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        file.write(id.toUtf8() + '\n');
        return id;
    }
    // Made meanwhile by pinholed (or another window): its, once written.
    for (int i = 0; i < 50; ++i) {
        if (const QString theirs = kept(); !theirs.isEmpty())
            return theirs;
        QThread::msleep(20);
    }
    qWarning("pinhole: %s stays empty", qPrintable(path));
    return id;
}

SyncedStore::Doc SyncedStore::read(const QString &path, bool *ok) {
    Doc doc;
    if (ok)
        *ok = true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // None yet is empty; one that cannot be read is not.
        if (ok && file.exists())
            *ok = false;
        return doc;
    }
    QJsonParseError error;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &error);
    if (!json.isObject()) {
        qWarning("pinhole: %s is not a JSON object (%s): left as it is", qPrintable(path), qPrintable(error.errorString()));
        if (ok)
            *ok = false;
        return doc;
    }
    doc.values = json.object().value("values").toObject();
    const QJsonObject clock = json.object().value("clock").toObject();
    for (auto it = clock.begin(); it != clock.end(); ++it) {
        const QJsonArray stamp = it.value().toArray();
        doc.clock.insert(it.key(), Stamp{qint64(stamp.at(0).toDouble()), stamp.at(1).toString()});
    }
    return doc;
}

QJsonObject SyncedStore::load(const QString &libraryDir, const QString &plugin) {
    if (libraryDir.isEmpty())
        return {};
    return read(fileIn(folderFor(libraryDir), plugin)).values;
}

QStringList SyncedStore::merge(Doc &into, const Doc &from) {
    QStringList changed;
    for (auto it = from.clock.begin(); it != from.clock.end(); ++it) {
        const auto mine = into.clock.constFind(it.key());
        if (mine != into.clock.cend() && !(mine.value() < it.value()))
            continue;
        const QJsonValue before = into.values.value(it.key());
        const QJsonValue value = from.values.value(it.key());
        if (value.isUndefined() || value.isNull())
            into.values.remove(it.key());
        else
            into.values.insert(it.key(), value);
        into.clock.insert(it.key(), it.value());
        if (into.values.value(it.key()) != before)
            changed.append(it.key());
    }
    return changed;
}

SyncedStore::Doc &SyncedStore::doc(const QString &plugin) {
    auto it = m_docs.find(plugin);
    if (it == m_docs.end()) {
        Doc read;
        if (!m_dir.isEmpty()) {
            const QString path = fileIn(m_dir, plugin);
            // Looked at first: a change after it is read again.
            const Seen before = seen(path);
            read = SyncedStore::read(path);
            read.file = before;
        }
        it = m_docs.emplace(plugin, std::move(read)).first;
    }
    return it->second;
}

void SyncedStore::setLibrary(const QString &libraryDir) {
    const QString folder = libraryDir.isEmpty() ? QString() : folderFor(libraryDir);
    if (folder == m_dir)
        return;
    // What was set in the library before goes into its own files.
    if (m_writing)
        writeChanged();
    QHash<QString, QStringList> before;
    for (const auto &[plugin, doc] : m_docs)
        before.insert(plugin, doc.values.keys());
    m_docs.clear();
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    m_dir = folder;
    if (!m_dir.isEmpty()) {
        QDir().mkpath(m_dir);
        m_watcher.addPath(m_dir);
    }
    // Told at once, not at the end of the turn: what reads them next reads
    // this library's.
    m_told.clear();
    for (auto it = before.begin(); it != before.end(); ++it) {
        QStringList keys = it.value() + doc(it.key()).values.keys();
        keys.removeDuplicates();
        emit changed(it.key(), keys);
    }
}

QJsonObject SyncedStore::values(const QString &plugin) { return doc(plugin).values; }

void SyncedStore::set(const QString &plugin, const QString &key, const QJsonValue &value) {
    set(plugin, QJsonObject{{key, value.isUndefined() ? QJsonValue(QJsonValue::Null) : value}});
}

void SyncedStore::set(const QString &plugin, const QJsonObject &changes) {
    Doc &mine = doc(plugin);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList changed;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QJsonValue value = it.value().isUndefined() ? QJsonValue(QJsonValue::Null) : it.value();
        const bool gone = value.isNull();
        if (gone ? !mine.values.contains(it.key()) : mine.values.value(it.key()) == value)
            continue;
        // A millisecond later than it was last set, wherever: it wins over
        // whatever this computer knew of it.
        const auto was = mine.clock.constFind(it.key());
        const Stamp stamp{was == mine.clock.cend() ? now : std::max(now, was.value().at + 1), m_device};
        keep(plugin, it.key(), Change{value, stamp});
        changed.append(it.key());
    }
    if (changed.isEmpty())
        return;
    tell(plugin, changed);
}

void SyncedStore::keep(const QString &plugin, const QString &key, const Change &change) {
    Doc &mine = doc(plugin);
    if (change.value.isNull())
        mine.values.remove(key);
    else
        mine.values.insert(key, change.value);
    mine.clock.insert(key, change.stamp);
    m_pending[plugin].insert(key, change);
    // Written once whatever else is set in this turn has been.
    if (!std::exchange(m_writing, true))
        QMetaObject::invokeMethod(this, &SyncedStore::writeChanged, Qt::QueuedConnection);
}

void SyncedStore::writeChanged() {
    m_writing = false;
    const auto pending = std::exchange(m_pending, {});
    if (m_dir.isEmpty())
        return;
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        const QString plugin = it.key(), folder = m_dir, path = fileIn(m_dir, plugin);
        const QHash<QString, Change> changes = it.value();
        // What the file holds once written, and how it is then.
        auto written = std::make_shared<std::pair<Doc, Seen>>();
        m_files.run(
            [folder, path, changes, written, plugin, failed = m_failed] {
                const auto said = [&](bool ok) {
                    const std::lock_guard guard(failed->lock);
                    if (ok)
                        failed->plugins.remove(plugin);
                    else
                        failed->plugins.insert(plugin);
                    return ok;
                };
                const FolderLock lock(folder);
                bool ok = false;
                Doc file = read(path, &ok);
                if (!lock.held() || !ok) {
                    qWarning("pinhole: %s could not be read: what was set is not kept", qPrintable(path));
                    return said(false);
                }
                for (auto c = changes.begin(); c != changes.end(); ++c) {
                    const auto there = file.clock.constFind(c.key());
                    if (there != file.clock.cend() && !(there.value() < c.value().stamp))
                        continue;
                    if (c.value().value.isNull())
                        file.values.remove(c.key());
                    else
                        file.values.insert(c.key(), c.value().value);
                    file.clock.insert(c.key(), c.value().stamp);
                }
                QJsonObject clock;
                for (auto s = file.clock.begin(); s != file.clock.end(); ++s)
                    clock.insert(s.key(), QJsonArray{double(s.value().at), s.value().by});
                const QByteArray bytes =
                    QJsonDocument(QJsonObject{{"values", file.values}, {"clock", clock}}).toJson(QJsonDocument::Compact);
                if (!KeptFiles::write(path, bytes))
                    return said(false);
                written->first = std::move(file);
                written->second = seen(path);
                return said(true);
            },
            [self = QPointer<SyncedStore>(this), plugin, folder, written](bool ok) {
                if (!self)
                    return;
                if (!ok) {
                    emit self->failed(plugin);
                    return;
                }
                // What it wrote, pinholed's merged in before it with it,
                // taken in here, so the file need not be read again for it
                // (one written after it is).
                auto doc = self->m_docs.find(plugin);
                if (folder == self->m_dir && doc != self->m_docs.end()) {
                    if (const QStringList keys = merge(doc->second, written->first); !keys.isEmpty())
                        self->tell(plugin, keys);
                    doc->second.file = written->second;
                }
            });
    }
}

void SyncedStore::reread() {
    if (m_dir.isEmpty())
        return;
    for (auto &[plugin, doc] : m_docs) {
        const QString path = fileIn(m_dir, plugin);
        const Seen now = seen(path);
        if (now == doc.file)
            continue;
        bool ok = false;
        const Doc file = read(path, &ok);
        if (!ok)
            continue;
        doc.file = now;
        // The later of each key: what is set here and not yet written
        // stays.
        if (const QStringList keys = merge(doc, file); !keys.isEmpty())
            tell(plugin, keys);
    }
}

void SyncedStore::tell(const QString &plugin, const QStringList &keys) {
    m_told[plugin] += keys;
    if (std::exchange(m_telling, true))
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_telling = false;
            const auto told = std::exchange(m_told, {});
            for (auto it = told.begin(); it != told.end(); ++it) {
                QStringList keys = it.value();
                keys.removeDuplicates();
                emit changed(it.key(), keys);
            }
        },
        Qt::QueuedConnection);
}

void SyncedStore::flush() {
    if (m_writing)
        writeChanged();
    m_files.wait();
}

bool SyncedStore::kept(const QString &plugin) {
    flush();
    const std::lock_guard guard(m_failed->lock);
    return !m_dir.isEmpty() && !m_failed->plugins.contains(plugin);
}

PluginSyncedMap::PluginSyncedMap(SyncedStore *store, const QString &plugin, QObject *parent)
    : QQmlPropertyMap(this, parent), m_store(store), m_plugin(plugin) {
    // Each as JSON: read in JS, a copy (a list kept as a QVariantList
    // would be a view of the property, changing with the next set).
    const QJsonObject kept = store->values(plugin);
    QVariantHash all;
    all.reserve(kept.size());
    for (auto it = kept.begin(); it != kept.end(); ++it)
        all.insert(it.key(), QVariant::fromValue(QJsonValue(it.value())));
    insertAll(this, all);
    connect(store, &SyncedStore::changed, this, [this](const QString &plugin, const QStringList &keys) {
        if (plugin != m_plugin)
            return;
        const QJsonObject now = m_store->values(m_plugin);
        QVariantHash told;
        told.reserve(keys.size());
        for (const QString &key : keys) {
            const QJsonValue value = now.value(key);
            // Taken out: undefined, as one never set is.
            told.insert(key, value.isUndefined() ? QVariant() : QVariant::fromValue(value));
        }
        insertAll(this, told);
        emit changed(keys);
    });
}

QStringList PluginSyncedMap::keys() const { return m_store->values(m_plugin).keys(); }

QJsonObject PluginSyncedMap::values() const { return m_store->values(m_plugin); }

bool PluginSyncedMap::kept() { return m_store->kept(m_plugin); }

QVariant PluginSyncedMap::updateValue(const QString &key, const QVariant &input) {
    // A JS object or list as plain values, as JSON keeps them.
    const QVariant value = input.metaType() == QMetaType::fromType<QJSValue>()
                               ? input.value<QJSValue>().toVariant(QJSValue::ConvertJSObjects)
                               : input;
    const QJsonValue json = QJsonValue::fromVariant(value);
    m_store->set(m_plugin, key, json);
    return json.isNull() || json.isUndefined() ? QVariant() : QVariant::fromValue(json);
}
