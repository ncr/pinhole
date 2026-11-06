#include "plugins.h"
#include "pluginquery.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>

#include <cstdio>
#include <memory>

// Real PluginHost promises and lifecycle signals; the SQL never writes.
// Run on an isolated profile with PINHOLE_SELFTEST=querylimits.
void runQuerySelfTest(QQuickWindow *window) {
    QTimer::singleShot(1500, window, [window] {
        auto *engine = qmlEngine(window);
        auto *plugins = qobject_cast<Plugins *>(engine->rootContext()->contextProperty("plugins").value<QObject *>());
        auto *host = plugins ? qobject_cast<PluginHost *>(plugins->at("pinhole.recent").value("host").value<QObject *>()) : nullptr;
        if (!host) {
            std::fputs("selftest: querylimits: missing plugin host\n", stderr);
            return QCoreApplication::exit(1);
        }
        engine->globalObject().setProperty("queryTestHost", engine->newQObject(host));
        engine->evaluate(QStringLiteral(R"(
            var queryTestAnswers = [];
            var queryTestJobs = [];
            var queryTestSql = 'WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n) SELECT sum(x) FROM n';
            function queryTestStart(tag) {
                var job = queryTestHost.query(queryTestSql);
                queryTestJobs.push(job);
                job.then(() => queryTestAnswers.push(tag + ':unexpected-success'), e => queryTestAnswers.push(tag + ':' + e.code));
            }
            queryTestHost.query('SELECT ? AS value', ['bound value']).then(rows => queryTestAnswers.push(rows[0].value));
        )"));
        auto *timer = new QTimer(window);
        auto phase = std::make_shared<int>(0), ticks = std::make_shared<int>(0);
        auto clock = std::make_shared<QElapsedTimer>(); clock->start();
        const auto fail = [timer](const char *what) {
            std::fprintf(stderr, "selftest: querylimits: %s\n", what);
            timer->stop(); QCoreApplication::exit(1);
        };
        QObject::connect(timer, &QTimer::timeout, window, [=] {
            ++*ticks;
            if (clock->elapsed() > 10000) return fail("timed out");
            const auto answers = engine->globalObject().property("queryTestAnswers").toVariant().toStringList();
            switch (*phase) {
            case 0:
                if (!answers.contains("bound value")) return;
                engine->evaluate(QStringLiteral("queryTestStart('cancel');"));
                ++*phase; *ticks = 0; return;
            case 1:
                if (*ticks < 3) return; // event loop must stay responsive
                if (PluginQuery::outstanding() == 0) return fail("aggregate did not stay pending");
                engine->evaluate(QStringLiteral("queryTestJobs[0].cancel()"));
                ++*phase; return;
            case 2:
                if (!answers.contains("cancel:cancelled") || PluginQuery::outstanding() != 0) return;
                engine->evaluate(QStringLiteral("queryTestStart('reload');"));
                ++*phase; *ticks = 0; return;
            case 3:
                if (*ticks < 3) return;
                plugins->library()->reloaded();
                ++*phase; return;
            case 4:
                if (!answers.contains("reload:reloaded") || PluginQuery::outstanding() != 0) return;
                engine->evaluate(QStringLiteral("queryTestStart('one'); queryTestStart('two'); queryTestStart('three');"));
                ++*phase; return;
            case 5:
                if (!answers.contains("three:query_busy")) return;
                engine->evaluate(QStringLiteral("queryTestJobs[2].cancel(); queryTestJobs[3].cancel();"));
                ++*phase; return;
            case 6:
                if (!answers.contains("one:cancelled") || !answers.contains("two:cancelled") || PluginQuery::outstanding() != 0) return;
                engine->evaluate(QStringLiteral("queryTestStart('stopped');"));
                ++*phase; *ticks = 0; return;
            case 7:
                if (*ticks < 3) return;
                host->stop();
                ++*phase; *ticks = 0; return;
            default:
                if (PluginQuery::outstanding() != 0 || *ticks < 3) return;
                if (answers.contains("stopped:cancelled") || answers.contains("stopped:unexpected-success"))
                    return fail("stopped plugin received an obsolete callback");
                std::puts("selftest: querylimits: bound promise, responsive GUI, cancel, library reload, per-plugin admission and stop: PASS");
                timer->stop(); QCoreApplication::quit();
            }
        });
        timer->start(20);
    });
}
