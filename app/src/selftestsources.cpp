// PINHOLE_SELFTEST=sources:<dir>: the window's sources as they look, a
// frame of each in <dir> (selftest.h): the first run, the side panel's
// rows and what they ask, Add a source, and signing in to iCloud through
// to the code and the card saying a source needs signing in.

#include "selftest.h"

#include "library.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

void say(const QString &line) {
    std::printf("selftest: %s\n", qPrintable(line));
    std::fflush(stdout);
}

// A value of the library's meta in `dir` (listed_at, synced_at); empty if
// it has none.
QString metaOf(const QString &dir, const char *key) {
    QString value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("sourcesoff"));
        db.setDatabaseName(dir + "/library.sqlite");
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
            q.addBindValue(QString::fromLatin1(key));
            if (q.exec() && q.next())
                value = q.value(0).toString();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("sourcesoff"));
    return value;
}

void key(QQuickWindow *window, int code, const QString &text = {}) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier, text);
}

// Typed, a character at a time, into whatever has the keys.
void type(QQuickWindow *window, const QString &text) {
    for (const QChar c : text)
        key(window, c.toUpper().unicode(), QString(c));
}

} // namespace

void runSourcesSelfTest(QQuickWindow *window, const QString &dir) {
    auto find = [window](const char *name) { return window->findChild<QObject *>(QString::fromLatin1(name)); };
    auto grab = [window, dir](const QString &name) {
        window->grabWindow().save(dir + '/' + name + ".png");
        std::printf("selftest: %s\n", qPrintable(name));
        std::fflush(stdout);
    };
    auto show = [find](const char *name, bool shown) {
        if (QObject *o = find(name))
            o->setProperty("shown", shown);
    };
    // iCloud's sign-in: Pinhole's own panel before its plugin, then the
    // plugin's.
    auto openSignIn = [window, find] {
        if (QObject *panel = find("signIn")) {
            panel->setProperty("appleId", QString());
            panel->setProperty("shown", true);
            return;
        }
        QVariant icloud;
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, icloud), Q_ARG(QVariant, QStringLiteral("pinhole.icloud")));
        if (QObject *plugin = icloud.value<QObject *>())
            QMetaObject::invokeMethod(plugin, "openSignIn", Q_ARG(QVariant, QString()));
        else
            std::printf("selftest: no iCloud plugin\n");
    };
    // PINHOLE_SELFTEST_PICK: the key pressed on the first run ("Return",
    // "2"), if it shows.
    const QString pick = qEnvironmentVariable("PINHOLE_SELFTEST_PICK");
    using Step = std::pair<int, std::function<void()>>;
    auto steps = std::make_shared<std::vector<Step>>(std::vector<Step>{
        {1500, [=] {
             QObject *first = find("firstRun");
             const bool shown = first && first->property("shown").toBool();
             std::printf("selftest: first run %s\n", shown ? "shown" : "not shown");
             if (shown) {
                 grab("00-firstrun");
                 if (pick == QLatin1String("Return"))
                     key(window, Qt::Key_Return);
                 else if (pick.size() == 1)
                     type(window, pick);
             }
         }},
        {pick.isEmpty() ? 100 : 4000, [=] { grab("01-grid"); show("sidePanel", true); }},
        {1000, [=] { grab("02-side"); type(window, "o"); }},
        {400, [=] { grab("03-side-O"); key(window, Qt::Key_Escape); key(window, Qt::Key_Delete); }},
        {400, [=] { grab("04-side-remove"); key(window, Qt::Key_Escape); }},
        {300, [=] { show("sidePanel", false); show("addSource", true); }},
        {1500, [=] { grab("05-add"); show("addSource", false); }},
        {400, [=] { openSignIn(); }},
        {800, [=] { grab("06-signin"); type(window, "jane@example.com"); key(window, Qt::Key_Return); type(window, "two-factor"); }},
        {400, [=] { grab("07-typed"); key(window, Qt::Key_Return); }},
        {2500, [=] { grab("08-code"); type(window, "000000"); }},
        {2500, [=] { grab("09-wrong-code"); type(window, "123456"); }},
        {3000, [=] { grab("10-signed-in"); }},
    });
    auto run = std::make_shared<std::function<void(size_t)>>();
    *run = [=](size_t at) {
        if (at == steps->size()) {
            // Then the card saying it needs signing in, once the service
            // has found it does.
            auto waited = std::make_shared<int>(0);
            auto poll = std::make_shared<std::function<void()>>();
            *poll = [=] {
                QQuickItem *trouble = window->findChild<QQuickItem *>(QStringLiteral("trouble"));
                if ((trouble && trouble->isVisible()) || ++*waited > 40) {
                    std::printf("selftest: the card saying it needs signing in %s\n",
                                trouble && trouble->isVisible() ? "shown" : "NOT shown");
                    QTimer::singleShot(400, window, [=] {
                        grab("11-trouble");
                        QCoreApplication::quit();
                    });
                    return;
                }
                QTimer::singleShot(250, window, *poll);
            };
            (*poll)();
            return;
        }
        QTimer::singleShot((*steps)[at].first, window, [=] {
            (*steps)[at].second();
            (*run)(at + 1);
        });
    };
    (*run)(0);
}

void runSourcesOffSelfTest(QQuickWindow *window, const Library &library, const QString &dir) {
    auto grab = [window, dir](const QString &name) {
        window->grabWindow().save(dir + '/' + name + ".png");
        say(name);
    };
    auto count = [window] {
        QObject *grid = window->findChild<QObject *>(QStringLiteral("grid"));
        return grid ? grid->property("count").toInt() : -1;
    };
    auto shown = [window](const char *name) {
        QQuickItem *item = window->findChild<QQuickItem *>(QString::fromLatin1(name));
        return item && item->isVisible() && item->opacity() > 0;
    };
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    QObject *plugins = qmlEngine(window)->rootContext()->contextProperty("plugins").value<QObject *>();
    // Until `done` holds, polled every 50 ms, then `then` with how long it
    // took, or -1 after `within` ms.
    auto waitFor = [window](std::function<bool()> done, int within, std::function<void(qint64)> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto poll = std::make_shared<std::function<void()>>();
        *poll = [=] {
            if (done())
                return then(clock->elapsed());
            if (clock->elapsed() > within)
                return then(-1);
            QTimer::singleShot(50, window, [poll] { (*poll)(); });
        };
        (*poll)();
    };
    // The plugin the first button turns on, its library's folder and its
    // listing's meta, as they were with its photos shown.
    auto plugin = std::make_shared<QString>();
    auto folder = std::make_shared<QString>();
    auto listedAt = std::make_shared<QString>();
    auto syncedAt = std::make_shared<QString>();
    auto photos = std::make_shared<int>(0);
    auto finish = [=] {
        say(QStringLiteral("done, %1 failed").arg(*failures));
        QCoreApplication::exit(*failures ? 1 : 0);
    };
    // Its button's key, 1. One's own copy is not turned on by it: the side
    // panel opens at its switch, which says what trusting it means (frame
    // `frame`), and the switch turns it on.
    auto turnOn = [=](const QString &frame, std::function<void()> then) {
        key(window, Qt::Key_1, QStringLiteral("1"));
        QVariantMap info;
        QMetaObject::invokeMethod(plugins, "at", Q_RETURN_ARG(QVariantMap, info), Q_ARG(QString, *plugin));
        if (!info.value("personal").toBool())
            return then();
        QTimer::singleShot(800, window, [=] {
            QObject *side = window->findChild<QObject *>(QStringLiteral("sidePanel"));
            const bool opened = side->property("shown").toBool();
            check(opened && count() == 0,
                  QStringLiteral("1 on one's own copy (%1) opens the side panel at its switch (%2), turning nothing on (%3 photos)")
                      .arg(*plugin, opened ? "open" : "NOT open").arg(count()));
            grab(frame);
            QMetaObject::invokeMethod(plugins, "setEnabled", Q_ARG(QString, *plugin), Q_ARG(bool, true));
            side->setProperty("shown", false);
            then();
        });
    };
    // 6. Back on: its photos at once, and the source followed, not listed
    // again: a round after it came back (a source that can be listed
    // without signing in, such as a folder), the listing as before.
    auto backAgain = [=] {
        const int before = count();
        turnOn(QStringLiteral("4-copy-side"), [=] { waitFor([=] { return count() > 0; }, 30000, [=](qint64 ms) {
            check(before == 0 && ms >= 0 && count() > 0,
                  QStringLiteral("on again by its button: %1 photos back %2 ms after the key (%3 before it was off)").arg(count()).arg(ms).arg(*photos));
            grab("4-on-again");
            waitFor([=] { return metaOf(*folder, "synced_at").toLongLong() > syncedAt->toLongLong(); }, 60000, [=](qint64 ms) {
                const QString listed = metaOf(*folder, "listed_at");
                check(ms >= 0 && listed == *listedAt,
                      ms < 0 ? QStringLiteral("no round followed the source after it came back (synced at %1 still): nothing shows it was "
                                              "not listed again; run it on a source that is listed without signing in, a folder")
                                   .arg(*syncedAt)
                             : QStringLiteral("not listed again: listed at %1 before it was off and still after, followed at %2")
                                   .arg(*listedAt, metaOf(*folder, "synced_at")));
                finish();
            });
        }); });
    };
    // 5. Off again while the window is open: the photos go, the card says
    // so, and the grid's buttons go with them.
    auto offAgain = [=] {
        // As it is now: a round since its listing settled counts as before.
        *listedAt = metaOf(*folder, "listed_at");
        *syncedAt = metaOf(*folder, "synced_at");
        *photos = count();
        QMetaObject::invokeMethod(plugins, "setEnabled", Q_ARG(QString, *plugin), Q_ARG(bool, false));
        waitFor([=] { return count() == 0 && shown("sourcesOff"); }, 30000, [=](qint64 ms) {
            QTimer::singleShot(600, window, [=] {
                check(ms >= 0 && count() == 0 && shown("sourcesOff") && !shown("gridActions") && !shown("selectButton"),
                      QStringLiteral("turned off while open: the photos go %1 ms after, %2 in the grid, the card %3, the grid's buttons %4")
                          .arg(ms).arg(count()).arg(shown("sourcesOff") ? "shown" : "NOT shown")
                          .arg(shown("gridActions") || shown("selectButton") ? "STILL THERE" : "gone"));
                grab("3-off-again");
                // A second at least, so a round after it comes back is a
                // second on from the last one before.
                QTimer::singleShot(1200, window, backAgain);
            });
        });
    };
    // 4. Its listing settled: listed, and a round done since, with the
    // same photos for a moment.
    auto settled = [=] {
        *folder = library.dir();
        auto steady = std::make_shared<int>(-1);
        auto since = std::make_shared<QElapsedTimer>();
        since->start();
        waitFor([=] {
            if (count() != *steady) {
                *steady = count();
                since->restart();
            }
            return !metaOf(*folder, "listed_at").isEmpty() && !metaOf(*folder, "synced_at").isEmpty() && since->elapsed() > 3000;
        }, 120000, [=](qint64 ms) {
            *listedAt = metaOf(*folder, "listed_at");
            *syncedAt = metaOf(*folder, "synced_at");
            *photos = count();
            say(QStringLiteral("%1 photos in %2: listed at %3, followed at %4 (settled %5 ms on)")
                    .arg(*photos).arg(*folder, listedAt->isEmpty() ? QStringLiteral("never") : *listedAt, *syncedAt).arg(ms));
            grab("2-on");
            QTimer::singleShot(1200, window, offAgain);
        });
    };
    // 1. At the start, its plugin off: the card on its own backdrop and no
    // photo, nor any of the grid's buttons.
    QTimer::singleShot(2500, window, [=] {
        const bool card = shown("sourcesOff");
        const bool buttons = shown("gridActions") || shown("selectButton") || shown("zoomButtons");
        check(card && count() == 0 && !buttons,
              QStringLiteral("the plugins being off said in the middle, %1 photos in the grid, its buttons %2")
                  .arg(count()).arg(buttons ? "STILL THERE" : "gone"));
        grab("0-off");
        // A Repeater's buttons are its parent item's children, not the
        // window's QObjects.
        std::function<void(QQuickItem *)> look = [&](QQuickItem *item) {
            if (item->objectName().startsWith(QLatin1String("turnOn:")) && item->property("key").toString() == QLatin1String("1"))
                *plugin = item->objectName().mid(7);
            for (QQuickItem *child : item->childItems())
                look(child);
        };
        look(window->contentItem());
        if (!card || plugin->isEmpty()) {
            check(false, QStringLiteral("a Turn on button on 1"));
            return finish();
        }
        // 2. The side panel at its switch.
        QObject *side = window->findChild<QObject *>(QStringLiteral("sidePanel"));
        side->setProperty("shown", true);
        QTimer::singleShot(800, window, [=] {
            QMetaObject::invokeMethod(side, "showPlugin", Q_ARG(QVariant, *plugin));
            QTimer::singleShot(600, window, [=] {
                grab("1-side");
                side->setProperty("shown", false);
                QTimer::singleShot(500, window, [=] {
                    // 3. Turn it on. None were there before it (checked
                    // above): these are its.
                    turnOn(QStringLiteral("1-copy-side"), [=] {
                        waitFor([=] { return count() > 0; }, 30000, [=](qint64 ms) {
                            check(ms >= 0, QStringLiteral("%1 photos back %2 ms after it was turned on (%3)").arg(count()).arg(ms).arg(*plugin));
                            if (ms < 0)
                                return finish();
                            settled();
                        });
                    });
                });
            });
        });
    });
}
