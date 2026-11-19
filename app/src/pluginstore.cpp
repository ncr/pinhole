#include "pluginstore.h"

#include "propertymap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

KeptFiles::KeptFiles(QObject *parent) : QObject(parent) {
    // One at a time, so what was asked for last is what is there.
    m_pool.setMaxThreadCount(1);
}

KeptFiles::~KeptFiles() { m_pool.waitForDone(); }

void KeptFiles::run(std::function<bool()> job, std::function<void(bool)> done) {
    m_pool.start([this, job = std::move(job), done = std::move(done)] {
        const bool ok = job();
        if (done)
            QMetaObject::invokeMethod(this, [done, ok] { done(ok); }, Qt::QueuedConnection);
    });
}

void KeptFiles::wait() { m_pool.waitForDone(); }

bool KeptFiles::write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
        return true;
    qWarning("pinhole: could not keep %s: %s", qPrintable(path), qPrintable(file.errorString()));
    return false;
}

PluginStore::PluginStore(const QString &root, const QString &name, KeptFiles *files, QObject *parent)
    : QObject(parent), m_root(root), m_name(name), m_files(files) {
    // Written by rename: the folder says so.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &PluginStore::reread);
}

void PluginStore::follow(const QString &id) {
    const QString folder = QFileInfo(path(id)).absolutePath();
    if (QFileInfo(folder).isDir() && !m_watcher.directories().contains(folder))
        m_watcher.addPath(folder);
}

void PluginStore::reread(const QString &folder) {
    const QString id = QFileInfo(folder).fileName();
    auto it = m_values.find(id);
    if (it == m_values.end())
        return;
    QFile file(path(id));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return;
    const QJsonObject there = document.object();
    QJsonObject unsaved;
    {
        const std::lock_guard guard(m_unsaved->lock);
        unsaved = m_unsaved->keys.value(id);
    }
    QStringList keys = there.keys() + it->second.keys();
    keys.removeDuplicates();
    QStringList changed;
    for (const QString &key : keys) {
        if (unsaved.contains(key) || there.value(key) == it->second.value(key))
            continue;
        if (there.contains(key))
            it->second.insert(key, there.value(key));
        else
            it->second.remove(key);
        changed.append(key);
    }
    if (!changed.isEmpty())
        emit this->changed(id, changed);
}

QString PluginStore::path(const QString &id) const { return m_root + '/' + id + '/' + m_name; }

QJsonObject PluginStore::values(const QString &id) {
    auto it = m_values.find(id);
    if (it == m_values.end()) {
        QJsonObject read;
        QFile file(path(id));
        if (file.open(QIODevice::ReadOnly)) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
            if (document.isObject())
                read = document.object();
            else
                qWarning("pinhole: %s is not a JSON object (%s): left out", qPrintable(file.fileName()),
                         qPrintable(error.errorString()));
        }
        it = m_values.emplace(id, read).first;
        follow(id);
    }
    return it->second;
}

void PluginStore::set(const QString &id, const QString &key, const QJsonValue &value) {
    values(id);
    QJsonObject &mine = m_values[id];
    if (value.isNull() || value.isUndefined()) {
        if (!mine.contains(key))
            return;
        mine.remove(key);
    } else {
        if (mine.contains(key) && mine.value(key) == value)
            return;
        mine.insert(key, value);
    }
    {
        const std::lock_guard guard(m_unsaved->lock);
        m_unsaved->keys[id].insert(key, value.isUndefined() ? QJsonValue(QJsonValue::Null) : value);
    }
    // Written once whatever else is changed in this turn has been.
    if (!std::exchange(m_writing, true))
        QMetaObject::invokeMethod(this, &PluginStore::writeChanged, Qt::QueuedConnection);
}

QByteArray PluginStore::merged(const QString &id, const QJsonObject &more) {
    values(id);
    QJsonObject &mine = m_values[id];
    for (auto it = more.begin(); it != more.end(); ++it)
        if (it.value().isNull() || it.value().isUndefined())
            mine.remove(it.key());
        else
            mine.insert(it.key(), it.value());
    // Whatever was waiting to be written is in these bytes.
    const std::lock_guard guard(m_unsaved->lock);
    m_unsaved->keys.remove(id);
    return QJsonDocument(mine).toJson();
}

void PluginStore::writeChanged() {
    m_writing = false;
    QHash<QString, QJsonObject> pending;
    {
        const std::lock_guard guard(m_unsaved->lock);
        pending = m_unsaved->keys;
    }
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        const QString id = it.key(), file = path(id);
        const QJsonObject changes = it.value();
        // Followed from before its first write: another window's after it
        // is heard of.
        QDir().mkpath(QFileInfo(file).absolutePath());
        follow(id);
        const QPointer<PluginStore> self(this);
        m_files->run(
            [file, changes, id, unsaved = m_unsaved] {
                // The folder's lock, while the file is read, merged into
                // and written again: another window's keys stay.
                const QString folder = QFileInfo(file).absolutePath();
                QDir().mkpath(folder);
                const int fd = ::open(QFile::encodeName(folder).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (fd >= 0)
                    ::flock(fd, LOCK_EX);
                // As it is now: one there that cannot be read is never
                // written over (what others keep in it would go).
                QJsonObject now;
                bool readable = true;
                if (QFileInfo::exists(file)) {
                    QFile was(file);
                    const QJsonDocument document =
                        was.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(was.readAll()) : QJsonDocument();
                    readable = document.isObject();
                    now = document.object();
                }
                if (!readable)
                    qWarning("pinhole: %s could not be read: what was set is not kept", qPrintable(file));
                for (auto c = changes.begin(); c != changes.end(); ++c)
                    if (c.value().isNull())
                        now.remove(c.key());
                    else
                        now.insert(c.key(), c.value());
                const bool ok = readable && KeptFiles::write(file, QJsonDocument(now).toJson());
                if (fd >= 0) {
                    ::flock(fd, LOCK_UN);
                    ::close(fd);
                }
                // Written: no longer to write, unless set again since.
                if (ok) {
                    const std::lock_guard guard(unsaved->lock);
                    QJsonObject &left = unsaved->keys[id];
                    for (auto c = changes.begin(); c != changes.end(); ++c)
                        if (left.contains(c.key()) && left.value(c.key()) == c.value())
                            left.remove(c.key());
                    if (left.isEmpty())
                        unsaved->keys.remove(id);
                }
                return ok;
            },
            [self, id](bool ok) {
                if (!self)
                    return;
                if (!ok)
                    emit self->failed(id);
            });
    }
}

void PluginStore::flush() {
    if (m_writing)
        writeChanged();
    m_files->wait();
}

bool PluginStore::kept(const QString &id) {
    m_files->wait();
    const auto left = [&] {
        const std::lock_guard guard(m_unsaved->lock);
        return m_unsaved->keys.contains(id);
    };
    // What failed before, tried again now.
    if (left())
        writeChanged();
    m_files->wait();
    return !left();
}

PluginStoreMap::PluginStoreMap(PluginStore *store, const QString &id, QObject *parent)
    : QQmlPropertyMap(this, parent), m_store(store), m_id(id) {
    const QJsonObject kept = store->values(id);
    QVariantHash all;
    all.reserve(kept.size());
    for (auto it = kept.begin(); it != kept.end(); ++it)
        all.insert(it.key(), it.value().toVariant());
    insertAll(this, all);
    connect(store, &PluginStore::changed, this, [this](const QString &id, const QStringList &keys) {
        if (id != m_id)
            return;
        const QJsonObject now = m_store->values(m_id);
        QVariantHash told;
        told.reserve(keys.size());
        for (const QString &key : keys)
            told.insert(key, now.contains(key) ? now.value(key).toVariant() : QVariant());
        insertAll(this, told);
        emit changed(keys);
    });
}

QStringList PluginStoreMap::keys() const { return m_store->values(m_id).keys(); }

QJsonObject PluginStoreMap::values() const { return m_store->values(m_id); }

bool PluginStoreMap::kept() { return m_store->kept(m_id); }

QVariant PluginStoreMap::updateValue(const QString &key, const QVariant &input) {
    // A JS object or list as plain values, as JSON keeps them.
    const QVariant value = input.metaType() == QMetaType::fromType<QJSValue>()
                               ? input.value<QJSValue>().toVariant(QJSValue::ConvertJSObjects)
                               : input;
    const QJsonValue json = QJsonValue::fromVariant(value);
    m_store->set(m_id, key, json);
    // Taken out: undefined, as one never set is.
    return json.isNull() || json.isUndefined() ? QVariant() : value;
}
