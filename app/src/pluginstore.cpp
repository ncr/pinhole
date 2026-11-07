#include "pluginstore.h"

#include "propertymap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>

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
    : QObject(parent), m_root(root), m_name(name), m_files(files) {}

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
    m_changed.insert(id);
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
    m_changed.remove(id);
    return QJsonDocument(mine).toJson();
}

void PluginStore::writeChanged() {
    m_writing = false;
    const QSet<QString> changed = std::exchange(m_changed, {});
    for (const QString &id : changed) {
        const QString file = path(id);
        const QByteArray bytes = QJsonDocument(m_values[id]).toJson();
        const QPointer<PluginStore> self(this);
        m_files->run([file, bytes] { return KeptFiles::write(file, bytes); }, [self, id](bool ok) {
            if (!ok && self)
                emit self->failed(id);
        });
    }
}

void PluginStore::flush() {
    if (m_writing)
        writeChanged();
    m_files->wait();
}

PluginStoreMap::PluginStoreMap(PluginStore *store, const QString &id, QObject *parent)
    : QQmlPropertyMap(this, parent), m_store(store), m_id(id) {
    const QJsonObject kept = store->values(id);
    QVariantHash all;
    all.reserve(kept.size());
    for (auto it = kept.begin(); it != kept.end(); ++it)
        all.insert(it.key(), it.value().toVariant());
    insertAll(this, all);
}

QStringList PluginStoreMap::keys() const { return m_store->values(m_id).keys(); }

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
