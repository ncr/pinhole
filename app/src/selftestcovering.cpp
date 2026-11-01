// PINHOLE_SELFTEST=covering: one plugin over the photos at a time
// (selftest.h).

#include "selftest.h"

#include "keymap.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
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
        return plugin && plugin->property("covering").toBool();
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
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=](int k) {
        if (k >= steps->size()) {
            say(QStringLiteral("done, %1 failed").arg(*failures));
            QCoreApplication::exit(*failures ? 1 : 0);
            return;
        }
        (*steps)[k].act();
        // Long enough for the viewer to open or close, and a plugin's
        // screen to fade.
        QTimer::singleShot(1200, window, [=] {
            const Step &step = (*steps)[k];
            state(step.name, step.layer, step.covering);
            check(shown("pinhole.onthisday") == step.days && shown("pinhole.colour") == step.colours
                      && shown("pinhole.calendar") == step.calendar,
                  QStringLiteral("%1: On this day %2, Colours %3, Calendar %4")
                      .arg(step.name, step.days ? "over the photos" : "away", step.colours ? "over the photos" : "away",
                           step.calendar ? "over the photos" : "away"));
            (*run)(k + 1);
        });
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
