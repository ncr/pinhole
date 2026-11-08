// PINHOLE_SELFTEST=covering: one plugin over the photos at a time, and
// Pinhole's own over it (selftest.h).

#include "selftest.h"

#include "photogrid.h"

#include "keymap.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <cstdio>
#include <functional>
#include <memory>

namespace {

void press(QQuickWindow *window, int code) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier);
}

// The Loader Main.qml loads a plugin in.
QQuickItem *loaderOf(QQuickItem *item, const QString &id) {
    if (item->property("pluginId").toString() == id)
        return item;
    for (QQuickItem *child : item->childItems())
        if (QQuickItem *found = loaderOf(child, id))
            return found;
    return nullptr;
}

// Whether `a` is drawn over `b`: by the first of their ancestors that are
// siblings, the higher z, else the later child.
bool drawnAbove(QQuickItem *a, QQuickItem *b) {
    QList<QQuickItem *> up, other;
    for (QQuickItem *item = a; item; item = item->parentItem())
        up.prepend(item);
    for (QQuickItem *item = b; item; item = item->parentItem())
        other.prepend(item);
    qsizetype k = 0;
    while (k < up.size() && k < other.size() && up[k] == other[k])
        ++k;
    if (k == 0 || k >= up.size() || k >= other.size())
        return false;
    if (up[k]->z() != other[k]->z())
        return up[k]->z() > other[k]->z();
    const QList<QQuickItem *> children = up[k - 1]->childItems();
    return children.indexOf(up[k]) > children.indexOf(other[k]);
}

// How much of `item`'s place on the window changed from `before` to
// `after`, 0 to 1.
double changed(const QImage &before, const QImage &after, QQuickItem *item) {
    if (before.size() != after.size() || before.isNull())
        return 0;
    const qreal ratio = before.devicePixelRatio();
    const QRect place = QRectF(item->mapToScene(QPointF(0, 0)) * ratio, item->size() * ratio).toAlignedRect()
                            .intersected(before.rect());
    qsizetype differ = 0, all = 0;
    for (int y = place.top(); y <= place.bottom(); y += 2)
        for (int x = place.left(); x <= place.right(); x += 2) {
            const QRgb p = before.pixel(x, y), q = after.pixel(x, y);
            differ += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q)) > 24;
            ++all;
        }
    return all > 0 ? double(differ) / all : 0;
}

// Pinhole's own over a plugin covering, On this day's: the help, the side
// panel, the undo notice and a plugin's notice (pinhole.notify), each
// drawn above it, by the items' stacking and by the window grabbed.
void overCovering(QQuickWindow *window, std::function<void(bool, const QString &)> check, std::function<void()> done) {
    QObject *plugins = qmlEngine(window)->rootContext()->contextProperty("plugins").value<QObject *>();
    QObject *history = qmlEngine(window)->rootContext()->contextProperty("history").value<QObject *>();
    auto item = [window](const char *name) { return window->findChild<QQuickItem *>(QString::fromLatin1(name)); };
    // How much of its place each changes at least: the help and the side
    // panel are see-through, so their words and lines (about a tenth and a
    // twentieth of it in a software-rendered 1600 × 1000 window).
    struct Over {
        QString name;
        const char *item;
        std::function<void()> show, hide;
        double least;
    };
    auto overs = std::make_shared<QList<Over>>(QList<Over>{
        // Made the first time it is asked for (Main.qml's showHelp).
        {"the help (?)", "help", [=] { QMetaObject::invokeMethod(window, "showHelp", Q_ARG(QVariant, true)); },
         [=] { QMetaObject::invokeMethod(window, "showHelp", Q_ARG(QVariant, false)); }, 0.05},
        {"the side panel (Tab)", "sidePanel", [=] { item("sidePanel")->setProperty("shown", true); },
         [=] { item("sidePanel")->setProperty("shown", false); }, 0.03},
        {"the undo notice (Ctrl+Z)", "historyNotice", [=] { QMetaObject::invokeMethod(history, "undo"); },
         [=] { item("historyNotice")->setProperty("opacity", 0); }, 0.02},
        {"a plugin's notice (pinhole.notify)", "historyNotice",
         [=] {
             QMetaObject::invokeMethod(plugins, "notice", Q_ARG(QString, QStringLiteral("pinhole.onthisday")),
                                       Q_ARG(QString, QStringLiteral("Saved to Pictures, as a plugin says")));
         },
         [=] { item("historyNotice")->setProperty("opacity", 0); }, 0.02},
    });
    press(window, Qt::Key_D);
    auto step = std::make_shared<std::function<void(int)>>();
    *step = [=](int k) {
        if (k >= overs->size()) {
            press(window, Qt::Key_Escape);
            QTimer::singleShot(1200, window, done);
            return;
        }
        const Over &over = (*overs)[k];
        const QImage before = window->grabWindow();
        over.show();
        QTimer::singleShot(900, window, [=] {
            const Over &over = (*overs)[k];
            QQuickItem *covering = loaderOf(window->contentItem(), QStringLiteral("pinhole.onthisday"));
            QQuickItem *own = item(over.item);
            const QString by = plugins->property("coveredBy").toString();
            check(by == QLatin1String("pinhole.onthisday") && covering && own && own->isVisible()
                      && drawnAbove(own, covering),
                  QStringLiteral("%1 over On this day covering (\"%2\"): above it").arg(over.name, by));
            const double part = own ? changed(before, window->grabWindow(), own) : 0;
            check(part >= over.least, QStringLiteral("%1 over On this day: %2% of its place drawn anew (%3% at least)")
                                         .arg(over.name).arg(qRound(part * 100)).arg(qRound(over.least * 100)));
            over.hide();
            QTimer::singleShot(900, window, [=] { (*step)(k + 1); });
        });
    };
    QTimer::singleShot(1500, window, [=] { (*step)(0); });
}

} // namespace

void runCoveringSelfTest(QQuickWindow *window, Viewer &viewer, Keymap &keymap) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto failures = std::make_shared<int>(0);
    auto check = [say, failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    auto shown = [window](const char *id) {
        QQuickItem *loader = loaderOf(window->contentItem(), QString::fromLatin1(id));
        QObject *plugin = loader ? loader->property("item").value<QObject *>() : nullptr;
        // Away while a photo it opened to come back from shows: covering
        // then counts for nothing (PluginPlane).
        return plugin && plugin->property("covering").toBool() && !loader->property("away").toBool();
    };
    auto seen = [window](const char *id) {
        QQuickItem *loader = loaderOf(window->contentItem(), QString::fromLatin1(id));
        return loader && loader->isVisible();
    };
    // Where things are: whose the keys are, which plugin covers and which
    // of the two show; and no key two live commands answer to.
    auto state = [=, &viewer, &keymap](const QString &name, const QString &layer, const QString &covering) {
        const QStringList clashes = keymap.clashes();
        say(QStringLiteral("%1: keys %2, covering \"%3\", On this day %4%5, Colours %6%7, Calendar %8%9, viewer %10")
                .arg(name, keymap.layer(), keymap.plugin())
                .arg(shown("pinhole.onthisday") ? "covering" : "away", seen("pinhole.onthisday") ? "" : " (unseen)")
                .arg(shown("pinhole.colour") ? "covering" : "away", seen("pinhole.colour") ? "" : " (unseen)")
                .arg(shown("pinhole.calendar") ? "covering" : "away", seen("pinhole.calendar") ? "" : " (unseen)")
                .arg(viewer.isOpen() ? "open" : "closed"));
        check(keymap.layer() == layer && keymap.plugin() == covering,
              QStringLiteral("%1: the keys are %2's%3").arg(name, layer, covering.isEmpty() ? QString() : ", " + covering + "'s"));
        check(clashes.isEmpty(), QStringLiteral("%1: no key shared%2").arg(name, clashes.isEmpty() ? QString() : ": " + clashes.join("; ")));
        if (!covering.isEmpty())
            for (const char *id : {"pinhole.onthisday", "pinhole.colour", "pinhole.calendar"})
                if (QString::fromLatin1(id) != covering)
                    check(!seen(id), QStringLiteral("%1: %2 unseen beneath").arg(name, QString::fromLatin1(id)));
    };
    // Each step: what to do, and then where things should be.
    struct Step {
        QString name;
        std::function<void()> act;
        QString layer, covering;
        bool days, colours, calendar = false;
    };
    auto triggerGridAction = [window](const QString &keyName) {
        // A plugin's grid button, as its key would press it, though
        // another plugin covers.
        for (const QVariant &a : window->property("pluginActions").toList()) {
            QObject *action = a.value<QObject *>();
            if (action && action->property("place").toString() == "grid" && action->property("key").toString() == keyName) {
                QMetaObject::invokeMethod(action, "triggered");
                return;
            }
        }
    };
    auto steps = std::make_shared<QList<Step>>(QList<Step>{
        {"On this day (D)", [=] { press(window, Qt::Key_D); }, "plugin", "pinhole.onthisday", true, false},
        {"a photo from it (⏎)", [=] { press(window, Qt::Key_Return); }, "viewer", "", false, false},
        {"Colours' Same colour on it (H)", [=] { press(window, Qt::Key_H); }, "plugin", "pinhole.colour", false, true},
        {"a photo from that (⏎)", [=] { press(window, Qt::Key_Return); }, "viewer", "", false, false},
        {"the photo closed (Esc): Colours alone comes back", [=] { press(window, Qt::Key_Escape); }, "plugin", "pinhole.colour", false, true},
        {"Colours closed (Esc): the grid, not On this day", [=] { press(window, Qt::Key_Escape); }, "grid", "", false, false},
        {"On this day again (D)", [=] { press(window, Qt::Key_D); }, "plugin", "pinhole.onthisday", true, false},
        {"Colours over it: the last to come covers", [=] { triggerGridAction(QStringLiteral("H")); }, "plugin", "pinhole.colour", true, true},
        {"Colours closed (Esc): On this day's again", [=] { press(window, Qt::Key_Escape); }, "plugin", "pinhole.onthisday", true, false},
        {"On this day closed (Esc)", [=] { press(window, Qt::Key_Escape); }, "grid", "", false, false},
        // Left by ⏎ from the one on top: those beneath close for good.
        {"On this day once more (D)", [=] { press(window, Qt::Key_D); }, "plugin", "pinhole.onthisday", true, false},
        {"Colours over it again", [=] { triggerGridAction(QStringLiteral("H")); }, "plugin", "pinhole.colour", true, true},
        {"a photo from Colours (⏎): On this day closed for good", [=] { press(window, Qt::Key_Return); }, "viewer", "", false, false},
        {"the photo closed (Esc): Colours alone comes back", [=] { press(window, Qt::Key_Escape); }, "plugin", "pinhole.colour", false, true},
        {"Colours closed (Esc): the grid, not On this day", [=] { press(window, Qt::Key_Escape); }, "grid", "", false, false},
        {"On this day (D) for the calendar", [=] { press(window, Qt::Key_D); }, "plugin", "pinhole.onthisday", true, false},
        {"the Year calendar over it", [=] { triggerGridAction(QStringLiteral("Y")); }, "plugin", "pinhole.calendar", true, false, true},
        {"a day from it (⏎): the grid, On this day closed for good", [=] { press(window, Qt::Key_Return); }, "grid", "", false, false},
    });
    PhotoGrid *grid = window->findChild<PhotoGrid *>(QStringLiteral("grid"));
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=, &viewer, &keymap](int k) {
        if (k >= steps->size()) {
            // Then Pinhole's own over a plugin covering.
            overCovering(window, check, [=] {
                say(QStringLiteral("done, %1 failed").arg(*failures));
                QCoreApplication::exit(*failures ? 1 : 0);
            });
            return;
        }
        (*steps)[k].act();
        // Once the viewer has opened or closed and the grid is still (a
        // plugin's screen fades as long as they move, half a second), however
        // slowly frames come.
        auto settled = std::make_shared<std::function<void(int)>>();
        *settled = [=, &viewer, &keymap](int polls) {
            // Where it should be, and still there (frames, not a guess):
            // or what it is after eight seconds, said as it is.
            const Step &want = (*steps)[k];
            const bool there = keymap.layer() == want.layer && keymap.plugin() == want.covering
                               && shown("pinhole.onthisday") == want.days && shown("pinhole.colour") == want.colours
                               && shown("pinhole.calendar") == want.calendar && !viewer.animating() && !grid->animating();
            static int stillFor = 0;
            stillFor = there ? stillFor + 1 : 0;
            if (stillFor < 10 && polls < 400) {
                QTimer::singleShot(20, window, [settled, polls] { (*settled)(polls + 1); });
                return;
            }
            stillFor = 0;
            const Step &step = (*steps)[k];
            state(step.name, step.layer, step.covering);
            check(shown("pinhole.onthisday") == step.days && shown("pinhole.colour") == step.colours
                      && shown("pinhole.calendar") == step.calendar,
                  QStringLiteral("%1: On this day %2, Colours %3, Calendar %4")
                      .arg(step.name, step.days ? "over the photos" : "away", step.colours ? "over the photos" : "away",
                           step.calendar ? "over the photos" : "away"));
            (*run)(k + 1);
        };
        (*settled)(0);
    };
    // Once both are loaded and On this day has years to show.
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto wait = std::make_shared<QTimer>();
    wait->setInterval(250);
    QObject::connect(wait.get(), &QTimer::timeout, window, [=] {
        window->setProperty("troubleSeen", true);
        QQuickItem *days = loaderOf(window->contentItem(), QStringLiteral("pinhole.onthisday"));
        QObject *plugin = days ? days->property("item").value<QObject *>() : nullptr;
        const bool ready = plugin && plugin->property("tourReady").toBool()
                           && loaderOf(window->contentItem(), QStringLiteral("pinhole.colour"))
                           && loaderOf(window->contentItem(), QStringLiteral("pinhole.calendar"));
        if (!ready && clock->elapsed() < 60000)
            return;
        wait->stop();
        if (!ready) {
            say(QStringLiteral("FAIL On this day, Colours and the Calendar not all ready in 60 s (no photos on this day?)"));
            QCoreApplication::exit(1);
            return;
        }
        (*run)(0);
    });
    wait->start();
}
