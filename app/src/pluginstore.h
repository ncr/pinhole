#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlPropertyMap>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <functional>
#include <map>

// Files written away from the window's thread, one after another in the
// order they were asked for, each in one step (written beside it, then
// renamed over it): what is kept as it changes never holds up a frame, and
// never lands half written.
class KeptFiles : public QObject {
    Q_OBJECT
public:
    explicit KeptFiles(QObject *parent = nullptr);
    // Waits for whatever is still being written.
    ~KeptFiles() override;

    // `job` on the writing thread, after every job asked for before it;
    // `done(ok)` with what it returned, back on this one.
    void run(std::function<bool()> job, std::function<void(bool)> done = {});
    // Everything asked for so far written.
    void wait();
    // One file in one step, its folder made first: false (said on stderr)
    // if it could not be.
    static bool write(const QString &path, const QByteArray &bytes);

private:
    QThreadPool m_pool;
};

// What plugins keep beside their settings, each plugin's a JSON object of
// keys of its own in a file of its own, `<root>/<id>/<name>`: its state,
// what the user made (~/.local/state/pinhole/plugins/<id>/state.json), and
// its cache, what it can work out again
// (~/.cache/pinhole/plugins/<id>/cache.json, beside its cacheDirs). A
// plugin's is read when first asked for. A change is there at once, and
// written once whatever else changes in the same turn of the event loop
// has, by KeptFiles.
class PluginStore : public QObject {
    Q_OBJECT
public:
    PluginStore(const QString &root, const QString &name, KeptFiles *files, QObject *parent = nullptr);

    QString path(const QString &id) const;
    // Plugin `id`'s, read the first time.
    QJsonObject values(const QString &id);
    // Changed at once and written soon; null or undefined takes it out.
    void set(const QString &id, const QString &key, const QJsonValue &value);
    // Several of plugin `id`'s at once, none of them written yet: the
    // file's bytes to write instead, taken as written (Plugins' moving of
    // settings, which writes them before plugins.json).
    QByteArray merged(const QString &id, const QJsonObject &values);
    // Every change so far written.
    void flush();

signals:
    // A change to plugin `id`'s could not be written.
    void failed(const QString &id);

private:
    void writeChanged();

    QString m_root, m_name;
    KeptFiles *m_files;
    std::map<QString, QJsonObject> m_values;
    QSet<QString> m_changed;
    bool m_writing = false;
};

// A plugin's state or cache as properties (pinhole.state, pinhole.cache),
// as pinhole.settings is: read one, bind to one, assign one and it is kept.
class PluginStoreMap : public QQmlPropertyMap {
    Q_OBJECT
public:
    PluginStoreMap(PluginStore *store, const QString &id, QObject *parent = nullptr);
    // Those it has: one set to null is taken out (QQmlPropertyMap keeps
    // a key once it has had it, as undefined).
    Q_INVOKABLE QStringList keys() const;

protected:
    QVariant updateValue(const QString &key, const QVariant &input) override;

private:
    PluginStore *m_store;
    QString m_id;
};
