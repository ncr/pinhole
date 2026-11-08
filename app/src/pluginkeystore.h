#pragma once

#include <QJSValue>
#include <QObject>
#include <QString>

struct sqlite3;
class PluginHost;

// What a plugin keeps a value for each of many keys of (pinhole.store):
// Info's facts of each photo's original, say. One SQLite file for
// each name in the plugin's cacheDir, so for each source's library apart,
// `<cacheDir>/<name>.store.sqlite`; a value is JSON, read and written one
// key at a time, so neither is the whole file each time, and two windows
// write theirs without losing the other's. What it holds can be worked
// out again: it goes with the cache.
class PluginKeyStore : public QObject {
    Q_OBJECT
public:
    PluginKeyStore(PluginHost *host, const QString &name);
    ~PluginKeyStore() override;

    // A promise of the value kept for `key`, null if none.
    Q_INVOKABLE QJSValue get(const QString &key);
    // `value` kept for `key` (null or undefined takes it out): a promise
    // once it is.
    Q_INVOKABLE QJSValue put(const QString &key, const QJSValue &value);
    Q_INVOKABLE QJSValue remove(const QString &key);

    // A name it can be kept under: lowercase letters, digits, '_' and '-'.
    static bool usableName(const QString &name);

private:
    // The file of the library shown now, opened if it is not; null, and
    // why, if it cannot be.
    sqlite3 *database(QString *why);
    void close();

    PluginHost *m_host;
    QString m_name;
    QString m_path;
    sqlite3 *m_db = nullptr;
};
