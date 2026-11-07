#include "pluginkeystore.h"

#include "plugins.h"

#include <QDir>
#include <QJSEngine>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <sqlite3.h>

PluginKeyStore::PluginKeyStore(PluginHost *host, const QString &name) : QObject(host), m_host(host), m_name(name) {}

PluginKeyStore::~PluginKeyStore() { close(); }

bool PluginKeyStore::usableName(const QString &name) {
    static const QRegularExpression usable(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,31}$"));
    return usable.match(name).hasMatch();
}

void PluginKeyStore::close() {
    if (m_db)
        sqlite3_close(m_db);
    m_db = nullptr;
    m_path.clear();
}

sqlite3 *PluginKeyStore::database(QString *why) {
    // Another library shown: its own file.
    const QString path = m_host->cacheDir() + '/' + m_name + QStringLiteral(".store.sqlite");
    if (m_db && path == m_path)
        return m_db;
    close();
    if (!QDir().mkpath(QFileInfo(path).path())) {
        *why = QStringLiteral("could not make %1").arg(QFileInfo(path).path());
        return nullptr;
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        *why = QString::fromUtf8(db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return nullptr;
    }
    // Two windows at once: each waits a moment for the other's write.
    sqlite3_busy_timeout(db, 2000);
    char *error = nullptr;
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS kept (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
                     nullptr, nullptr, &error) != SQLITE_OK) {
        *why = QString::fromUtf8(error ? error : "could not be made");
        sqlite3_free(error);
        sqlite3_close(db);
        return nullptr;
    }
    m_db = db;
    m_path = path;
    return m_db;
}

namespace {
// One statement, its text bound in order, stepped once: SQLITE_ROW's first
// column in `column`.
int once(sqlite3 *db, const char *sql, const QList<QByteArray> &bound, QByteArray *column = nullptr) {
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
    if (result != SQLITE_OK)
        return result;
    for (int i = 0; i < bound.size(); ++i)
        sqlite3_bind_text(statement, i + 1, bound[i].constData(), int(bound[i].size()), SQLITE_TRANSIENT);
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW && column)
        *column = QByteArray(static_cast<const char *>(sqlite3_column_blob(statement, 0)), sqlite3_column_bytes(statement, 0));
    sqlite3_finalize(statement);
    return result;
}
} // namespace

QJSValue PluginKeyStore::get(const QString &key) {
    QString why;
    // At once: nothing to cancel.
    const auto d = m_host->defer([] {});
    const QJSValue promise = d.promise;
    sqlite3 *db = database(&why);
    if (!db) {
        m_host->settle(m_host->m_generation, d.reject, m_host->error(QStringLiteral("unavailable"), why));
        return promise;
    }
    QByteArray text;
    const int result = once(db, "SELECT value FROM kept WHERE key = ?1", {key.toUtf8()}, &text);
    if (result == SQLITE_DONE) {
        m_host->settle(m_host->m_generation, d.resolve, QJSValue(QJSValue::NullValue));
    } else if (result == SQLITE_ROW) {
        QJSEngine *js = qjsEngine(this);
        const QJsonDocument value = QJsonDocument::fromJson("[" + text + "]");
        m_host->settle(m_host->m_generation, d.resolve, js ? js->toScriptValue(value.array().at(0).toVariant()) : QJSValue());
    } else {
        m_host->settle(m_host->m_generation, d.reject, m_host->error(QStringLiteral("unavailable"), QString::fromUtf8(sqlite3_errmsg(db))));
    }
    return promise;
}

QJSValue PluginKeyStore::put(const QString &key, const QJSValue &value) {
    if (value.isNull() || value.isUndefined())
        return remove(key);
    QString why;
    // At once: nothing to cancel.
    const auto d = m_host->defer([] {});
    const QJSValue promise = d.promise;
    sqlite3 *db = database(&why);
    if (!db) {
        m_host->settle(m_host->m_generation, d.reject, m_host->error(QStringLiteral("unavailable"), why));
        return promise;
    }
    // As JSON: an object, a list, a string, a number or true or false.
    const QByteArray text = QJsonDocument(QJsonArray{QJsonValue::fromVariant(value.toVariant())}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
    const int result = once(db, "INSERT OR REPLACE INTO kept (key, value) VALUES (?1, ?2)", {key.toUtf8(), text});
    if (result == SQLITE_DONE)
        m_host->settle(m_host->m_generation, d.resolve, QJSValue(QJSValue::UndefinedValue));
    else
        m_host->settle(m_host->m_generation, d.reject, m_host->error(QStringLiteral("unavailable"), QString::fromUtf8(sqlite3_errmsg(db))));
    return promise;
}

QJSValue PluginKeyStore::remove(const QString &key) {
    QString why;
    // At once: nothing to cancel.
    const auto d = m_host->defer([] {});
    const QJSValue promise = d.promise;
    sqlite3 *db = database(&why);
    if (!db) {
        m_host->settle(m_host->m_generation, d.reject, m_host->error(QStringLiteral("unavailable"), why));
        return promise;
    }
    const int result = once(db, "DELETE FROM kept WHERE key = ?1", {key.toUtf8()});
    if (result == SQLITE_DONE)
        m_host->settle(m_host->m_generation, d.resolve, QJSValue(QJSValue::UndefinedValue));
    else
        m_host->settle(m_host->m_generation, d.reject, m_host->error(QStringLiteral("unavailable"), QString::fromUtf8(sqlite3_errmsg(db))));
    return promise;
}
