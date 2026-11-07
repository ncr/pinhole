// PINHOLE_SELFTEST=snappy[:<file>]: what must be at once is (selftest.h).
// With the library and plugin data the run was given (app/bin/test-speed
// gives a large library and plugins' data of years: thousands of faces
// named, say), each step is timed by how long the window stood still: a
// timer ticking every few milliseconds on the window's thread, and the
// longest it waited between ticks while the step ran and settled. The steps:
// the library read again and swapped in, five times, as a sync does; a photo
// opened full window and closed; and every plugin's view, each opened and
// closed as its key does. One line for each ("selftest: snappy: {...}"), all
// of them into <file> as JSON. Exits 1 if any stood still longer than
// PINHOLE_SNAPPY_MS (150 by default; a frame and a few on a slow runner).
// PINHOLE_SNAPPY_ONLY=<text>: only the steps whose name has it ("Y view",
// "reload"), to look at one closely (a profiler, gdb).

#include "selftest.h"

#include "library.h"
#include "libraryreloader.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

namespace {

// The longest the window's thread went without coming back to its events.
class Stillness : public QObject {
public:
    explicit Stillness(QObject *parent) : QObject(parent) {
        m_tick.setInterval(4);
        m_tick.setTimerType(Qt::PreciseTimer);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            m_longest = std::max(m_longest, m_since.restart());
        });
    }
    void begin() {
        m_longest = 0;
        m_since.start();
        m_tick.start();
    }
    // Including the one it is in now.
    qint64 longest() const { return std::max(m_longest, m_since.elapsed()); }

private:
    QTimer m_tick;
    QElapsedTimer m_since;
    qint64 m_longest = 0;
};

// Each view a plugin has (PluginView.qml): one with a key of its own.
// The tree of items, as each is drawn in another (a plugin's root is its
// Loader's).
void viewsIn(QQuickItem *item, QList<QQuickItem *> &views) {
    const QVariant key = item->property("toggleKey");
    if (key.isValid() && !key.toString().isEmpty() && item->property("shown").isValid()
        && item->metaObject()->indexOfMethod("toggle()") >= 0)
        views.append(item);
    for (QQuickItem *child : item->childItems())
        viewsIn(child, views);
}

QString viewName(QQuickItem *view) {
    return view->property("toggleKey").toString() + ' '
           + (view->objectName().isEmpty() ? QStringLiteral("view") : view->objectName());
}

QList<QQuickItem *> pluginViews(QQuickWindow *window) {
    QList<QQuickItem *> views;
    viewsIn(window->contentItem(), views);
    return views;
}

} // namespace

void runSnappySelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, LibraryReloader &reloader,
                       const QString &file) {
    const int budget = qEnvironmentVariableIsSet("PINHOLE_SNAPPY_MS") ? qEnvironmentVariableIntValue("PINHOLE_SNAPPY_MS") : 150;
    auto *still = new Stillness(window);
    auto results = std::make_shared<QJsonArray>();
    auto failures = std::make_shared<int>(0);
    auto steps = std::make_shared<QList<std::function<void(std::function<void()>)>>>();

    const auto record = [=](const QString &step, qint64 ms, QJsonObject more = {}) {
        more.insert("step", step);
        more.insert("still_ms", ms);
        more.insert("budget_ms", budget);
        const bool ok = ms <= budget;
        more.insert("ok", ok);
        if (!ok)
            ++*failures;
        results->append(more);
        std::printf("selftest: snappy: %s\n", QJsonDocument(more).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
    };
    // What a step started settles for this long, timed with it: work it
    // put off to the next turns stands the window still as well.
    const auto settle = [window](int ms, std::function<void()> then) { QTimer::singleShot(ms, window, then); };
    const QString only = qEnvironmentVariable("PINHOLE_SNAPPY_ONLY");
    const auto wanted = [only](const QString &step) { return only.isEmpty() || step.contains(only); };

    // The library read again, as a sync does, five times.
    for (int i = 0; i < (wanted(QStringLiteral("reload")) ? 5 : 0); ++i)
        steps->append([=, &reloader](std::function<void()> next) {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = QObject::connect(&reloader, &LibraryReloader::swapped, window,
                                           [=](qint64 readMs, qint64 swapMs) {
                QObject::disconnect(*connection);
                settle(300, [=] {
                    record(QStringLiteral("reload"), still->longest(), {{"swap_ms", swapMs}, {"read_ms", readMs}});
                    next();
                });
            });
            still->begin();
            reloader.reload();
        });

    // A photo full window, and back.
    steps->append([=, &viewer, &library](std::function<void()> next) {
        if (library.count() == 0 || !wanted(QStringLiteral("viewer"))) {
            next();
            return;
        }
        still->begin();
        viewer.openAt(library.count() / 2, QRectF(0, 0, 100, 100));
        settle(600, [=, &viewer] {
            record(QStringLiteral("viewer open"), still->longest());
            still->begin();
            viewer.close();
            settle(600, [=] {
                record(QStringLiteral("viewer close"), still->longest());
                next();
            });
        });
    });

    // Every plugin's view, opened and closed.
    steps->append([=](std::function<void()> next) {
        auto views = std::make_shared<QList<QPointer<QQuickItem>>>();
        const QList<QQuickItem *> all = pluginViews(window);
        // None found is a test that times nothing: said, and a failure.
        std::printf("selftest: snappy: %lld plugin views\n", static_cast<long long>(all.size()));
        if (all.isEmpty())
            ++*failures;
        for (QQuickItem *view : all)
            if (wanted(viewName(view)))
                views->append(view);
        auto each = std::make_shared<std::function<void()>>();
        *each = [=] {
            if (views->isEmpty()) {
                next();
                return;
            }
            QPointer<QQuickItem> view = views->takeFirst();
            if (!view) {
                (*each)();
                return;
            }
            const QString name = viewName(view);
            still->begin();
            QMetaObject::invokeMethod(view, "toggle");
            settle(600, [=] {
                record(QStringLiteral("open ") + name, still->longest());
                // One that did not open is not closed: no close timed.
                const bool shown = view && view->property("shown").toBool();
                still->begin();
                if (shown)
                    QMetaObject::invokeMethod(view, "close");
                else
                    std::printf("selftest: snappy: %s did not open\n", qPrintable(name));
                settle(400, [=] {
                    if (shown)
                        record(QStringLiteral("close ") + name, still->longest());
                    (*each)();
                });
            });
        };
        (*each)();
    });

    auto run = std::make_shared<std::function<void()>>();
    *run = [=] {
        if (steps->isEmpty()) {
            if (!file.isEmpty()) {
                QFile out(file);
                if (out.open(QIODevice::WriteOnly))
                    out.write(QJsonDocument(QJsonObject{{"photos", library.count()}, {"steps", *results}}).toJson());
            }
            std::printf("selftest: snappy: %s\n", *failures ? "FAIL" : "PASS");
            std::fflush(stdout);
            QCoreApplication::exit(*failures ? 1 : 0);
            return;
        }
        steps->takeFirst()([run] { QTimer::singleShot(100, [run] { (*run)(); }); });
    };
    // Once it is up and has drawn: what starting costs is measured apart
    // (app/bin/measure-startup).
    QTimer::singleShot(1500, window, [run] { (*run)(); });
    QTimer::singleShot(120000, window, [] {
        std::fprintf(stderr, "selftest: snappy: timed out\n");
        QCoreApplication::exit(1);
    });
}
