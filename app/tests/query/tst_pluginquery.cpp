#include "pluginquery.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJSEngine>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <sqlite3.h>

class PluginQueryTests : public QObject {
    Q_OBJECT
    struct Library {
        QTemporaryDir dir;
        sqlite3 *writer = nullptr;
        Library() {
            sqlite3_open(QFile::encodeName(path()).constData(), &writer);
            sqlite3_exec(writer, "CREATE TABLE assets(id TEXT, value INTEGER); INSERT INTO assets VALUES ('a',1),('b',2),('c',3)", nullptr, nullptr, nullptr);
        }
        ~Library() { sqlite3_close(writer); }
        QString path() const { return dir.path() + "/library.sqlite"; }
    };
    const QString endlessQuery = QStringLiteral("WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n) SELECT sum(x) FROM n");

    static PluginQuery::Answer read(const Library &library, const QString &sql, const QVariantList &values = {},
                                    PluginQuery::Limits limits = {}) {
        return PluginQuery::read(library.path(), sql, values, std::make_shared<PluginQuery::Control>(limits.milliseconds), limits);
    }

private slots:
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(PluginQuery::outstanding(), 0, 3000); }

    void bindingsAndColumnTypes() {
        Library library;
        const QString injected = "'; ATTACH DATABASE '/tmp/never-created-by-query' AS other; --";
        const auto answer = read(library, "SELECT ? AS text, ? AS number, ? AS real, ? AS absent, ? AS bytes",
                                  {injected, qlonglong(1234567890123), 1.25, QVariant(), QByteArray("a\0b", 3)});
        QCOMPARE(answer.code, QString());
        QCOMPARE(answer.rows.size(), 1);
        const auto row = answer.rows[0].toMap();
        QCOMPARE(row.value("text").toString(), injected);
        QCOMPARE(row.value("number").toLongLong(), 1234567890123LL);
        QCOMPARE(row.value("real").toDouble(), 1.25);
        QVERIFY(row.value("absent").isNull());
        QCOMPARE(row.value("bytes").toByteArray(), QByteArray("a\0b", 3));
        const auto empty = read(library, "SELECT '' AS text, CAST('' AS BLOB) AS bytes, NULL AS absent").rows[0].toMap();
        QVERIFY(!empty.value("text").toString().isNull());
        QVERIFY(!empty.value("bytes").toByteArray().isNull());
        QVERIFY(empty.value("absent").isNull());
        QCOMPARE(read(library, "SELECT id FROM assets WHERE value > ? ORDER BY value", {1}).rows.size(), 2);
        QCOMPARE(read(library, "SELECT ?", {}).code, QString("sql"));
    }

    void sqlNullAndEmptyValuesKeepTheExistingQtSqlJavaScriptShape() {
        Library library;
        QCOMPARE(sqlite3_exec(library.writer,
            "CREATE TABLE nullable(i INTEGER, t TEXT, b BLOB); INSERT INTO nullable VALUES(NULL,NULL,NULL)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        const QStringList queries{
            "SELECT NULL AS plain, CAST(NULL AS TEXT) AS text_null, CAST(NULL AS BLOB) AS blob_null, "
            "CAST(NULL AS INTEGER) AS int_null, '' AS empty_text, CAST('' AS BLOB) AS empty_blob",
            "SELECT i AS int_null, t AS text_null, b AS blob_null FROM nullable"
        };
        QJSEngine js;
        for (const auto &sql : queries) {
            QVariantMap previous;
            const QString name = "query-null-compatibility";
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", name);
                db.setDatabaseName(library.path());
                db.setConnectOptions("QSQLITE_OPEN_READONLY");
                QVERIFY(db.open());
                QSqlQuery query(db);
                QVERIFY(query.exec(sql));
                QVERIFY(query.next());
                const auto columns = query.record();
                for (int i = 0; i < columns.count(); ++i)
                    previous.insert(columns.fieldName(i), query.value(i));
            }
            QSqlDatabase::removeDatabase(name);
            const auto answer = read(library, sql);
            QCOMPARE(answer.code, QString());
            QCOMPARE(answer.rows.size(), 1);
            const auto current = answer.rows[0].toMap();
            const auto oldJs = js.toScriptValue(QVariantList{previous}).property(0);
            const auto newJs = js.toScriptValue(answer.rows).property(0);
            QCOMPARE(current.keys(), previous.keys());
            for (auto column = previous.begin(); column != previous.end(); ++column) {
                const auto value = current.value(column.key());
                QCOMPARE(value.metaType(), column->metaType());
                QCOMPARE(value.isNull(), column->isNull());
                const auto oldValue = oldJs.property(column.key()), newValue = newJs.property(column.key());
                QCOMPARE(newValue.isUndefined(), oldValue.isUndefined());
                QCOMPARE(newValue.isNull(), oldValue.isNull());
                QCOMPARE(newValue.isString(), oldValue.isString());
                QCOMPARE(newValue.toVariant(), oldValue.toVariant());
            }
            QVERIFY(!newJs.property("text_null").isUndefined());
            QVERIFY(newJs.property("text_null").isString());
            QCOMPARE(newJs.property("text_null").toString(), QString(""));
        }
    }

    void readOnlyMeansNoWriteAttachOrConnectionChanges() {
        Library library;
        const QString outside = library.dir.path() + "/attached.sqlite";
        const QStringList refused{
            "INSERT INTO assets VALUES ('d',4)", "DELETE FROM assets", "CREATE TABLE extra(n)",
            "ATTACH DATABASE '" + outside + "' AS extra", "PRAGMA user_version=7", "PRAGMA query_only=OFF",
            "SELECT load_extension('/no/such/extension')", "BEGIN", "VACUUM INTO '" + outside + "'"
        };
        for (const auto &sql : refused) {
            const auto answer = read(library, sql);
            QCOMPARE(answer.code, QString("sql"));
            QVERIFY(answer.rows.isEmpty());
        }
        QVERIFY(!QFile::exists(outside));
        QCOMPARE(read(library, "SELECT count(*) AS n FROM assets").rows[0].toMap().value("n").toInt(), 3);
        QCOMPARE(read(library, "PRAGMA table_info(assets)").rows.size(), 2);
    }

    void onlyOneStatementButTrailingCommentsAreFine() {
        Library library;
        QCOMPARE(read(library, "SELECT 1 AS n; -- a comment\n /* another */").rows.size(), 1);
        QCOMPARE(read(library, "SELECT 1; SELECT 2").code, QString("sql"));
        QCOMPARE(read(library, "SELECT 1; INSERT INTO assets VALUES ('d',4)").code, QString("sql"));
        QCOMPARE(read(library, "-- only comments").code, QString("sql"));
        QCOMPARE(read(library, QString("SELECT 1") + QChar(0) + "; SELECT 2").code, QString("sql"));
    }

    void rowAndByteLimitsRejectWithoutPartialAnswers() {
        Library library;
        PluginQuery::Limits limits;
        limits.rows = 2;
        auto answer = read(library, "SELECT id FROM assets", {}, limits);
        QCOMPARE(answer.code, QString("query_limit"));
        QVERIFY(answer.rows.isEmpty());
        limits.rows = 3;
        QCOMPARE(read(library, "SELECT id FROM assets", {}, limits).rows.size(), 3);
        limits.bytes = 1600;
        answer = read(library, "SELECT printf('%0600d',value) AS text FROM assets", {}, limits);
        QCOMPARE(answer.code, QString("query_limit"));
        QVERIFY(answer.rows.isEmpty());
        limits = {};
        limits.cellBytes = 128;
        QCOMPARE(read(library, "SELECT randomblob(129)", {}, limits).code, QString("query_limit"));
        QCOMPARE(read(library, "SELECT ?", {QString(129, 'x')}, limits).code, QString("query_limit"));
        limits = {};
        limits.sqlBytes = 64;
        QCOMPARE(read(library, "SELECT 1 /*" + QString(100, 'x') + "*/", {}, limits).code, QString("query_limit"));
    }

    void wildcardPatternsCannotHideUnboundedWorkInsideOneOpcode() {
        Library library;
        const QString text(524288, 'a');
        const QString suffix = QString(40000, 'a') + 'b';
        QElapsedTimer elapsed; elapsed.start();
        for (const QString &op : {QString("LIKE"), QString("GLOB")}) {
            const QString pattern = (op == "LIKE" ? "%" : "*") + suffix;
            const auto answer = read(library, "SELECT ? " + op + " ?", {text, pattern});
            QCOMPARE(answer.code, QString("sql"));
            QVERIFY(answer.rows.isEmpty());
            QVERIFY(answer.message.contains("pattern too complex"));
        }
        QVERIFY2(elapsed.elapsed() < 1000, "wildcard function bypassed the execution budget");
        QCOMPARE(read(library, "SELECT ? LIKE ? AS matched", {"abc", "a%"}).rows[0].toMap().value("matched").toInt(), 1);
        QCOMPARE(read(library, "SELECT ? GLOB ? AS matched", {"abc", "a*"}).rows[0].toMap().value("matched").toInt(), 1);
        QCOMPARE(read(library, "SELECT ? LIKE ?", {"x", QString(1024, 'a')}).code, QString());
        QCOMPARE(read(library, "SELECT ? LIKE ?", {"x", QString(1025, 'a')}).code, QString("sql"));
    }

    void executionBudgetInterruptsBeforeTheFirstRow() {
        Library library;
        PluginQuery::Limits limits;
        limits.milliseconds = 50;
        QElapsedTimer elapsed; elapsed.start();
        const auto answer = read(library, endlessQuery, {}, limits);
        QCOMPARE(answer.code, QString("query_timeout"));
        QVERIFY(answer.rows.isEmpty());
        QVERIFY2(elapsed.elapsed() < 1000, "query was not interrupted during computation");
    }

    void cancellingComputationDoesNotWaitForARowOrBlockTheEventLoop() {
        Library library;
        const auto control = std::make_shared<PluginQuery::Control>();
        auto answer = PluginQuery::start(library.path(), endlessQuery, {}, control);
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(5);
        QTest::qWait(40);
        QVERIFY(ticks >= 2);
        QVERIFY(!answer.isFinished());
        QElapsedTimer elapsed; elapsed.start();
        control->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(answer.isFinished(), 1000);
        QCOMPARE(answer.result().code, QString("cancelled"));
        QVERIFY(answer.result().rows.isEmpty());
        QVERIFY(elapsed.elapsed() < 500);
    }

    void lockWaitHasTheSameDeadlineAndIsCancellable() {
        Library library;
        QCOMPARE(sqlite3_exec(library.writer, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr), SQLITE_OK);
        auto control = std::make_shared<PluginQuery::Control>(50);
        auto answer = PluginQuery::start(library.path(), "SELECT * FROM assets", {}, control);
        QTRY_VERIFY_WITH_TIMEOUT(answer.isFinished(), 1000);
        QCOMPARE(answer.result().code, QString("query_timeout"));
        control = std::make_shared<PluginQuery::Control>();
        answer = PluginQuery::start(library.path(), "SELECT * FROM assets", {}, control);
        QTest::qWait(20);
        control->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(answer.isFinished(), 1000);
        QCOMPARE(answer.result().code, QString("cancelled"));
        QCOMPARE(sqlite3_exec(library.writer, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
    }

    void admissionAndQueueWaitAreBounded() {
        Library library;
        QList<std::shared_ptr<PluginQuery::Control>> controls;
        QList<QFuture<PluginQuery::Answer>> answers;
        for (int i = 0; i < 8; ++i) {
            auto control = std::make_shared<PluginQuery::Control>();
            controls.append(control);
            answers.append(PluginQuery::start(library.path(), endlessQuery, {}, control));
        }
        auto rejected = PluginQuery::start(library.path(), "SELECT 1", {}, std::make_shared<PluginQuery::Control>());
        QVERIFY(rejected.isFinished());
        QCOMPARE(rejected.result().code, QString("query_busy"));
        for (const auto &control : controls) control->cancel();
        for (auto &answer : answers) {
            QTRY_VERIFY_WITH_TIMEOUT(answer.isFinished(), 1000);
            QCOMPARE(answer.result().code, QString("cancelled"));
        }
        QTRY_COMPARE(PluginQuery::outstanding(), 0);
        auto a = std::make_shared<PluginQuery::Control>(), b = std::make_shared<PluginQuery::Control>();
        auto first = PluginQuery::start(library.path(), endlessQuery, {}, a);
        auto second = PluginQuery::start(library.path(), endlessQuery, {}, b);
        auto queued = PluginQuery::start(library.path(), "SELECT 1", {}, std::make_shared<PluginQuery::Control>(25));
        QTest::qWait(60);
        a->cancel(); b->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(queued.isFinished(), 1000);
        QCOMPARE(queued.result().code, QString("query_timeout"));
        QTRY_VERIFY(first.isFinished() && second.isFinished());
    }

    void aHundredThousandPhotoIdsStillFitTheDefaultBudget() {
        Library library;
        QCOMPARE(sqlite3_exec(library.writer,
            "DELETE FROM assets; WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
            "INSERT INTO assets SELECT printf('%036d',x), x FROM n", nullptr, nullptr, nullptr), SQLITE_OK);
        const auto answer = read(library, "SELECT id FROM assets");
        QCOMPARE(answer.code, QString());
        QCOMPARE(answer.rows.size(), 100000);
    }
};

QTEST_GUILESS_MAIN(PluginQueryTests)
#include "tst_pluginquery.moc"
