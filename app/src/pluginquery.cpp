#include "pluginquery.h"

#include <QByteArray>
#include <QFile>
#include <QMetaType>
#include <QPromise>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <sqlite3.h>

#include <cstring>
#include <limits>
#include <utility>

namespace PluginQuery {
namespace {
std::atomic_int admitted{0};

QThreadPool &pool() {
    static QThreadPool workers;
    static const bool configured = [] {
        workers.setMaxThreadCount(2);
        workers.setExpiryTimeout(10000);
        return true;
    }();
    Q_UNUSED(configured);
    return workers;
}

struct Database {
    sqlite3 *value = nullptr;
    ~Database() { if (value) sqlite3_close(value); }
};
struct Statement {
    sqlite3_stmt *value = nullptr;
    ~Statement() { if (value) sqlite3_finalize(value); }
};

Answer failed(const QString &code, const QString &message = {}) { return {{}, code, message}; }

QString interrupted(const std::shared_ptr<Control> &control) {
    if (control->cancelled()) return QStringLiteral("cancelled");
    if (control->expired()) return QStringLiteral("query_timeout");
    return {};
}

Answer databaseError(sqlite3 *db, int result, const std::shared_ptr<Control> &control) {
    if (const auto code = interrupted(control); !code.isEmpty()) return failed(code);
    if (result == SQLITE_TOOBIG || result == SQLITE_NOMEM) return failed(QStringLiteral("query_limit"));
    return failed(QStringLiteral("sql"), QString::fromUtf8(sqlite3_errmsg(db)));
}

int progress(void *context) {
    auto *control = static_cast<Control *>(context);
    return control->cancelled() || control->expired();
}

int busy(void *context, int) {
    if (progress(context)) return 0;
    QThread::msleep(5);
    return !progress(context);
}

// The catalog's public tables (docs/plugins.md, Read-only SQL schema), and
// SQLite's own. A source's own tables (icloud_assets…) are its provider's:
// they change with it, and hold what is not a plugin's (download links).
bool isPublic(const char *table) {
    if (!table)
        return true;
    const QByteArray name = QByteArray(table).toLower();
    return name == "assets" || name == "renditions" || name == "meta" || name.startsWith("sqlite_");
}

int authorize(void *data, int action, const char *first, const char *second, const char *, const char *) {
    const bool everything = static_cast<const Limits *>(data)->privateTables;
    switch (action) {
    case SQLITE_SELECT:
    case SQLITE_RECURSIVE:
        return SQLITE_OK;
    case SQLITE_READ:
        return everything || isPublic(first) ? SQLITE_OK : SQLITE_DENY;
    case SQLITE_FUNCTION:
        return second && qstricmp(second, "load_extension") == 0 ? SQLITE_DENY : SQLITE_OK;
    case SQLITE_PRAGMA: {
        // Metadata introspection is read-only too. Never permit assigning
        // a pragma or changing memory/temp/durability configuration.
        const QByteArray name = first ? QByteArray(first).toLower() : QByteArray();
        static const QList<QByteArray> tableMetadata{"table_info", "table_xinfo", "index_list", "index_info", "index_xinfo", "foreign_key_list"};
        static const QList<QByteArray> metadata{"database_list", "schema_version", "user_version", "compile_options"};
        if (tableMetadata.contains(name))
            return everything || isPublic(second) ? SQLITE_OK : SQLITE_DENY;
        return !second && metadata.contains(name) ? SQLITE_OK : SQLITE_DENY;
    }
    default:
        // In particular, ATTACH is nominally a "read-only statement" to
        // SQLite but can create another database. The allowlist excludes
        // it, all writes/DDL and connection-changing commands.
        return SQLITE_DENY;
    }
}

int bind(sqlite3_stmt *statement, int at, const QVariant &value) {
    if (value.isNull() || !value.isValid()) return sqlite3_bind_null(statement, at);
    switch (value.metaType().id()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
        return sqlite3_bind_int64(statement, at, value.toLongLong());
    case QMetaType::ULongLong:
        return value.toULongLong() <= qulonglong(std::numeric_limits<sqlite3_int64>::max())
            ? sqlite3_bind_int64(statement, at, sqlite3_int64(value.toULongLong())) : SQLITE_MISMATCH;
    case QMetaType::Double:
    case QMetaType::Float:
        return sqlite3_bind_double(statement, at, value.toDouble());
    case QMetaType::QString: {
        const QByteArray text = value.toString().toUtf8();
        return sqlite3_bind_text64(statement, at, text.constData(), sqlite3_uint64(text.size()), SQLITE_TRANSIENT, SQLITE_UTF8);
    }
    case QMetaType::QByteArray: {
        const QByteArray bytes = value.toByteArray();
        return sqlite3_bind_blob64(statement, at, bytes.constData(), sqlite3_uint64(bytes.size()), SQLITE_TRANSIENT);
    }
    default:
        return SQLITE_MISMATCH;
    }
}
} // namespace

Control::Control(int milliseconds)
    : m_deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds)) {}

bool Control::expired() const { return std::chrono::steady_clock::now() >= m_deadline; }

Answer read(const QString &path, const QString &sql, const QVariantList &values,
            const std::shared_ptr<Control> &control, const Limits &limits) {
    if (const auto code = interrupted(control); !code.isEmpty()) return failed(code);
    const QByteArray source = sql.toUtf8();
    if (source.size() > limits.sqlBytes || values.size() > 256) return failed(QStringLiteral("query_limit"));
    if (source.contains('\0')) return failed(QStringLiteral("sql"), QStringLiteral("SQL contains a zero byte"));
    Database db;
    const QByteArray filename = QFile::encodeName(path);
    int result = sqlite3_open_v2(filename.constData(), &db.value, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    if (result != SQLITE_OK) return db.value ? databaseError(db.value, result, control)
                                             : failed(QStringLiteral("sql"), QStringLiteral("Could not open the library"));
    sqlite3_progress_handler(db.value, 1000, progress, control.get());
    sqlite3_busy_handler(db.value, busy, control.get());
    sqlite3_limit(db.value, SQLITE_LIMIT_LENGTH, limits.cellBytes);
    sqlite3_limit(db.value, SQLITE_LIMIT_SQL_LENGTH, limits.sqlBytes);
    sqlite3_limit(db.value, SQLITE_LIMIT_COLUMN, limits.columns);
    sqlite3_limit(db.value, SQLITE_LIMIT_VARIABLE_NUMBER, 256);
    sqlite3_limit(db.value, SQLITE_LIMIT_EXPR_DEPTH, 100);
    sqlite3_limit(db.value, SQLITE_LIMIT_COMPOUND_SELECT, 128);
    // LIKE/GLOB can spend quadratic time inside one native function call,
    // where SQLite's opcode progress handler cannot see the deadline.
    sqlite3_limit(db.value, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, 1024);
    sqlite3_db_config(db.value, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_set_authorizer(db.value, authorize, const_cast<Limits *>(&limits));
    Statement statement;
    const char *tail = nullptr;
    result = sqlite3_prepare_v3(db.value, source.constData(), int(source.size() + 1), 0, &statement.value, &tail);
    if (result != SQLITE_OK) return databaseError(db.value, result, control);
    if (!statement.value) return failed(QStringLiteral("sql"), QStringLiteral("No query to run"));
    if (!sqlite3_stmt_readonly(statement.value)) return failed(QStringLiteral("sql"), QStringLiteral("The library is read-only"));
    // Reject a second statement; comments and whitespace after one are fine.
    Statement another;
    result = sqlite3_prepare_v3(db.value, tail, -1, 0, &another.value, nullptr);
    if (result != SQLITE_OK) return databaseError(db.value, result, control);
    if (another.value) return failed(QStringLiteral("sql"), QStringLiteral("Use one query at a time"));
    if (sqlite3_bind_parameter_count(statement.value) != values.size())
        return failed(QStringLiteral("sql"), QStringLiteral("The number of bound values does not match"));
    qsizetype parameterBytes = 0;
    for (int i = 0; i < values.size(); ++i) {
        const auto &value = values[i];
        const qsizetype bytes = value.metaType().id() == QMetaType::QByteArray ? value.toByteArray().size()
            : value.metaType().id() == QMetaType::QString ? value.toString().size() * 2 : 16;
        parameterBytes += bytes;
        if (bytes > limits.cellBytes || parameterBytes > limits.bytes) return failed(QStringLiteral("query_limit"));
        result = bind(statement.value, i + 1, value);
        if (result == SQLITE_MISMATCH)
            return failed(QStringLiteral("sql"), QStringLiteral("Bound values must be null, numbers, strings or byte arrays"));
        if (result != SQLITE_OK) return databaseError(db.value, result, control);
    }
    QStringList names;
    qsizetype nameBytes = 0;
    const int columns = sqlite3_column_count(statement.value);
    for (int column = 0; column < columns; ++column) {
        names.append(QString::fromUtf8(sqlite3_column_name(statement.value, column)));
        nameBytes += names.last().size() * 2;
    }
    Answer answer;
    qsizetype keptBytes = 0;
    while (true) {
        if (const auto code = interrupted(control); !code.isEmpty()) return failed(code);
        result = sqlite3_step(statement.value);
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return databaseError(db.value, result, control);
        if (answer.rows.size() >= limits.rows) return failed(QStringLiteral("query_limit"));
        qsizetype rowBytes = 64 + columns * 96 + nameBytes;
        QVariantMap row;
        for (int column = 0; column < columns; ++column) {
            QVariant value;
            switch (sqlite3_column_type(statement.value, column)) {
            // QSQLite's historical query contract represents SQL NULL
            // with a typed-null QString; QJSEngine exposes it as "".
            // An invalid QVariant would silently change that to undefined.
            case SQLITE_NULL: value = QVariant(QMetaType::fromType<QString>()); break;
            case SQLITE_INTEGER: value = qlonglong(sqlite3_column_int64(statement.value, column)); break;
            case SQLITE_FLOAT: value = sqlite3_column_double(statement.value, column); break;
            case SQLITE_BLOB: {
                const int bytes = sqlite3_column_bytes(statement.value, column);
                rowBytes += bytes;
                if (bytes > limits.cellBytes || rowBytes > limits.bytes - keptBytes) return failed(QStringLiteral("query_limit"));
                value = bytes == 0 ? QByteArray("")
                                   : QByteArray(static_cast<const char *>(sqlite3_column_blob(statement.value, column)), bytes);
                break;
            }
            case SQLITE_TEXT: {
                const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement.value, column));
                const int bytes = sqlite3_column_bytes(statement.value, column);
                // UTF-16 takes at most twice the UTF-8 byte count; check
                // before allocating/copying a cell into Qt-owned memory.
                rowBytes += qsizetype(bytes) * 2;
                if (bytes > limits.cellBytes || rowBytes > limits.bytes - keptBytes) return failed(QStringLiteral("query_limit"));
                value = QString::fromUtf8(text, bytes);
                break;
            }
            }
            row.insert(names[column], value);
        }
        if (rowBytes > limits.bytes - keptBytes) return failed(QStringLiteral("query_limit"));
        keptBytes += rowBytes;
        answer.rows.append(row);
    }
    if (const auto code = interrupted(control); !code.isEmpty()) return failed(code);
    return answer;
}

QFuture<Answer> start(const QString &path, const QString &sql, const QVariantList &values,
                      const std::shared_ptr<Control> &control, const Limits &limits) {
    if (admitted.fetch_add(1) >= 8) {
        admitted.fetch_sub(1);
        control->finish();
        QPromise<Answer> ready;
        ready.start(); ready.addResult(failed(QStringLiteral("query_busy"))); ready.finish();
        return ready.future();
    }
    return QtConcurrent::run(&pool(), [path, sql, values, control, limits] {
        const Answer answer = read(path, sql, values, control, limits);
        control->finish();
        admitted.fetch_sub(1);
        return answer;
    });
}

int outstanding() { return admitted.load(); }

} // namespace PluginQuery
