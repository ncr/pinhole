// PINHOLE_SELFTEST=live:<dir>: Live Photos, with the Live Photos plugin
// (plugins/live) on or off (selftest.h).

#include "selftest.h"

#include "library.h"
#include "photogrid.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJSEngine>
#include <QPointer>
#include <QQmlContext>
#include <QQmlListReference>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVideoSink>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

namespace {

void press(QQuickWindow *window, int code, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString &text = {}) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, mods, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, mods, text);
}

QObject *pluginItem(QQuickWindow *window, const QString &id) {
    QVariant item;
    QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, item), Q_ARG(QVariant, id));
    return item.value<QObject *>();
}

QRectF inScene(QQuickItem *item) {
    return item ? item->mapRectToScene(QRectF(0, 0, item->width(), item->height())) : QRectF();
}

// The complaint said now under a button (Complaint.qml), "" for none.
QString complaint(QQuickItem *item) {
    if (item->objectName() == QLatin1String("complaint") && item->property("saying").toBool())
        return item->property("text").toString();
    for (QQuickItem *child : item->childItems())
        if (const QString said = complaint(child); !said.isEmpty())
            return said;
    return {};
}

} // namespace

void runLiveSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, Viewer &viewer, const QString &folder) {
    auto failures = std::make_shared<int>(0);
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto check = [say, failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    // The top of the window only: a whole one takes long enough to save
    // that the timers and animations here fall behind.
    auto grabTop = [window, folder](const QString &name) {
        const QImage frame = window->grabWindow();
        frame.copy(0, 0, frame.width(), frame.height() / 8).save(QStringLiteral("%1/%2.png").arg(folder, name));
    };
    auto grab = [window, folder](const QString &name) { window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name)); };
    auto live = [window] { return pluginItem(window, QStringLiteral("pinhole.live")); };
    auto layer = [live]() -> QObject * { return live() ? live()->property("liveLayer").value<QObject *>() : nullptr; };
    auto player = [layer]() -> QObject * {
        QObject *l = layer();
        QObject *motion = l ? l->property("motion").value<QObject *>() : nullptr;
        return motion ? motion->property("player").value<QObject *>() : nullptr;
    };
    auto button = [live]() -> QQuickItem * {
        QObject *action = live() ? live()->property("playAction").value<QObject *>() : nullptr;
        return action ? action->property("button").value<QQuickItem *>() : nullptr;
    };
    auto state = [window, layer, button] {
        QQuickItem *b = button();
        return QStringLiteral("loading %1, button %2%3, notice \"%4\"")
            .arg(layer() && layer()->property("asking").toBool())
            .arg(b && b->property("active").toBool() ? "lit" : "plain")
            .arg(b && b->property("busy").toBool() ? " busy" : "")
            .arg(complaint(window->contentItem()));
    };
    auto effect = [live] { return live() ? live()->property("effect").toString() : QString(); };
    // The layer as it is, for a line of the log.
    auto describe = [layer] {
        QObject *l = layer();
        if (!l)
            return QStringLiteral("no layer");
        return QStringLiteral("layer: effect %1, motion %2 (%3), playing %4, asking %5, failed \"%6\", long exposure %7")
            .arg(l->property("effect").toString(), l->property("source").toString().section('/', -1),
                 l->property("sourceKind").toString())
            .arg(l->property("playing").toBool())
            .arg(l->property("asking").toBool())
            .arg(l->property("failed").toString())
            .arg(l->property("longExposure").toString().isEmpty() ? "none" : "here");
    };
    auto playing = [layer] { return layer() && layer()->property("playing").toBool(); };
    // The effect chosen in its menu by the keys, as a person would: ⇧ L
    // opens it on the effect shown, ↓ ↑ to the one, ⏎.
    auto choose = [window, effect](const QString &to) {
        static const QStringList effects{"live", "loop", "bounce", "long_exposure"};
        press(window, Qt::Key_L, Qt::ShiftModifier, QStringLiteral("L"));
        const int steps = effects.indexOf(to) - std::max<qsizetype>(0, effects.indexOf(effect()));
        for (int i = 0; i < std::abs(steps); ++i)
            press(window, steps > 0 ? Qt::Key_Down : Qt::Key_Up);
        press(window, Qt::Key_Return);
    };
    auto menuOpen = [window] {
        QObject *bar = window->findChild<QObject *>(QStringLiteral("viewerActions"));
        return bar && bar->property("menuOpen").toBool();
    };
    auto frameOf = [window] { return window->findChild<QQuickItem *>(QStringLiteral("viewerFrame")); };

    // What other plugins fetch of a Live Photo (Background its loop and
    // bounce, Frame and Send its motion): through Background's own way in,
    // whether this plugin is on or not.
    QJSEngine *engine = qmlEngine(window);
    engine->globalObject().setProperty(QStringLiteral("selftestFetched"), engine->newArray());
    auto fetchVia = [window, engine](const QString &plugin, int photo, const QString &kind) {
        QObject *item = pluginItem(window, plugin);
        QObject *host = item ? item->property("pinhole").value<QObject *>() : nullptr;
        if (!host)
            return false;
        QQmlEngine::setObjectOwnership(host, QQmlEngine::CppOwnership);
        const QJSValue f = engine->evaluate(QStringLiteral(
            "(function (host, photo, kind) { host.fetch(photo, kind).then("
            "path => selftestFetched.push(kind + ' ' + path), e => selftestFetched.push(kind + ' failed: ' + e.code)) })"));
        f.call({engine->newQObject(host), photo, kind});
        return true;
    };

    auto steps = std::make_shared<QList<std::pair<int, std::function<void()>>>>();
    auto at = [steps](int ms, std::function<void()> f) { steps->append({ms, std::move(f)}); };
    const int index = findIndex(library, QStringLiteral("a.is_live"));
    const int looped = findIndex(library, QStringLiteral("a.live_effect = 'loop'"));
    auto clock = std::make_shared<QElapsedTimer>();
    // The bounce's position as it plays, and how often it went back to the start.
    auto wraps = std::make_shared<int>(0);
    auto lastPosition = std::make_shared<qint64>(-1);
    auto everPlayed = std::make_shared<bool>(false);
    // The layer and the button as Bounce could not be had, if so.
    auto bounceFailed = std::make_shared<QString>();
    auto bounceFailedRight = std::make_shared<bool>(false);
    auto done = [say, failures, engine] {
        const QStringList fetched = engine->globalObject().property(QStringLiteral("selftestFetched")).toVariant().toStringList();
        for (const QString &f : fetched)
            say(QStringLiteral("fetched by Background: ") + f);
        say(QStringLiteral("done, %1 failed").arg(*failures));
        QCoreApplication::exit(*failures ? 1 : 0);
    };
    auto fetchedOk = [engine](const QString &kind) {
        for (const QString &f : engine->globalObject().property(QStringLiteral("selftestFetched")).toVariant().toStringList())
            if (f.startsWith(kind + ' ') && !f.contains(QLatin1String("failed")) && QFileInfo::exists(f.section(' ', 1)))
                return true;
        return false;
    };

    at(0, [=, &viewer] {
        if (index < 0) {
            say(QStringLiteral("no Live Photo in the library"));
            QCoreApplication::exit(1);
            return;
        }
        say(QStringLiteral("the Live Photos plugin is %1").arg(live() ? "on" : "off"));
        for (const char *kind : {"loop", "bounce", "live"})
            check(fetchVia(QStringLiteral("pinhole.background"), index, QString::fromLatin1(kind)),
                  QStringLiteral("Background asks for the Live Photo's %1").arg(QString::fromLatin1(kind)));
        grid->reveal(index);
    });
    at(300, [=, &viewer] { viewer.openAt(index, grid->rectOf(index)); });

    if (!pluginItem(window, QStringLiteral("pinhole.live"))) {
        // Off: a Live Photo is its still, and nothing of Live is there.
        at(2500, [=, &viewer] {
            QObject *plugins = qmlEngine(window)->rootContext()->contextProperty(QStringLiteral("plugins")).value<QObject *>();
            bool marked = false;
            for (const QVariant &kind : plugins->property("markKinds").toList())
                marked |= kind.toMap().value("id").toString() == QLatin1String("pinhole.live/live");
            check(!marked, QStringLiteral("no Live mark among the grid's marks, nor its switch"));
            check(viewer.isOpen() && viewer.stage() == QLatin1String("original"),
                  QStringLiteral("the photo shows sharp, as its still (%1)").arg(viewer.stage()));
            QObject *bar = window->findChild<QObject *>(QStringLiteral("viewerActions"));
            bool liveButton = false;
            for (QQuickItem *b : bar ? bar->findChildren<QQuickItem *>() : QList<QQuickItem *>())
                liveButton |= b->isVisible() && b->property("text").toString() == QLatin1String("Live");
            check(!liveButton, QStringLiteral("no Live button in the viewer's bar"));
            press(window, Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
            press(window, Qt::Key_L, Qt::ShiftModifier, QStringLiteral("L"));
        });
        at(3200, [=, &viewer] {
            check(viewer.isOpen() && !menuOpen(), QStringLiteral("L and ⇧ L do nothing; the viewer stays"));
            grab(QStringLiteral("live-off"));
        });
        at(25000, [=] {
            for (const char *kind : {"loop", "bounce", "live"})
                check(fetchedOk(QString::fromLatin1(kind)), QStringLiteral("Background has the %1").arg(QString::fromLatin1(kind)));
            done();
        });
    } else {
        // The photo sharp first, as it would be by the time anyone presses.
        at(2800, [=] {
            onChange(live(), "liveLayer", window, [=] {
                if (QObject *l = layer())
                    onChange(l, "playing", l, [=] { *everPlayed |= playing(); });
            });
            if (QObject *l = layer())
                onChange(l, "playing", l, [=] { *everPlayed |= playing(); });
            clock->start();
            press(window, Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
            press(window, Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
            say(QStringLiteral("pressed L twice: %1").arg(state()));
            for (int ms : {100, 400, 1000, 2000, 3500, 5000})
                QTimer::singleShot(ms, window, [=] {
                    say(QStringLiteral("%1 ms: %2").arg(clock->elapsed(), 5).arg(state()));
                    grabTop(QStringLiteral("live-%1").arg(ms, 4, 10, QLatin1Char('0')));
                });
        });
        at(8800, [=] {
            check(*everPlayed || !complaint(window->contentItem()).isEmpty(),
                  QStringLiteral("L played the motion (or said why not: \"%1\")").arg(complaint(window->contentItem())));
            press(window, Qt::Key_L, Qt::ShiftModifier, QStringLiteral("L"));
        });
        at(9300, [=] {
            check(menuOpen(), QStringLiteral("⇧ L opens the effects"));
            QObject *action = live()->property("playAction").value<QObject *>();
            int rows = 0;
            const QQmlListReference actions(live(), "actions");
            for (qsizetype i = 0; i < actions.count(); ++i)
                rows += actions.at(i) != action && actions.at(i)->property("shown").toBool();
            check(rows == 4, QStringLiteral("… four of them (%1), set on the iPhone: %2").arg(rows).arg(live()->property("saved").toString()));
            const QImage frame = window->grabWindow();
            frame.copy(0, 0, frame.width(), frame.height() / 3).save(QStringLiteral("%1/live-menu.png").arg(folder));
            press(window, Qt::Key_Escape);
        });
        at(9800, [=, &viewer] {
            check(!menuOpen() && viewer.isOpen(), QStringLiteral("Esc closes the menu, the photo stays"));
            choose(QStringLiteral("bounce"));
            check(effect() == QLatin1String("bounce"), QStringLiteral("Bounce chosen in the menu by the keys"));
        });
        at(10000, [=] {
            // Round and round: the position going back to the start.
            QTimer *watch = new QTimer(window);
            watch->setInterval(20);
            QObject::connect(watch, &QTimer::timeout, window, [=] {
                QObject *p = player();
                if (!p || !playing())
                    return;
                const qint64 position = p->property("position").toLongLong();
                if (*lastPosition >= 0 && position + 300 < *lastPosition)
                    ++*wraps;
                *lastPosition = position;
            });
            watch->start();
            QTimer::singleShot(9000, watch, &QObject::deleteLater);
        });
        // The bounce drawn over the photo, moving: its frames reach its
        // picture there.
        auto before = std::make_shared<QImage>();
        auto frames = std::make_shared<int>(0);
        at(12000, [=] { *before = window->grabWindow(); });
        at(10500, [=] {
            QObject *l = layer();
            QObject *motion = l ? l->property("motion").value<QObject *>() : nullptr;
            QQuickItem *output = motion ? motion->property("output").value<QQuickItem *>() : nullptr;
            QVideoSink *sink = output ? output->property("videoSink").value<QVideoSink *>() : nullptr;
            if (sink)
                QObject::connect(sink, &QVideoSink::videoFrameChanged, window, [frames] { ++*frames; });
            say(QStringLiteral("the bounce's picture: %1, in the photo: %2")
                    .arg(output ? QStringLiteral("%1x%2%3").arg(output->width()).arg(output->height()).arg(output->isVisible() ? ", shown" : ", hidden") : QStringLiteral("none"))
                    .arg(output && QRectF(inScene(output)).intersects(inScene(frameOf())) ? "yes" : "no"));
        });
        at(13000, [=] {
            say(describe());
            const QImage now = window->grabWindow();
            now.save(QStringLiteral("%1/live-bounce.png").arg(folder));
            const QRect area = inScene(frameOf()).toRect().intersected(now.rect());
            qint64 changed = 0;
            for (int y = area.top(); y <= area.bottom(); y += 4)
                for (int x = area.left(); x <= area.right(); x += 4)
                    changed += std::abs(qGray(now.pixel(x, y)) - qGray(before->pixel(x, y))) > 24;
            const qint64 all = qint64(area.width() / 4) * (area.height() / 4);
            // A grab draws no video (offscreen, or on a workspace not
            // shown): the frames reaching the picture are the proof.
            check(*frames > 10, QStringLiteral("the bounce plays in its picture over the photo: %1 frames in 2.5 s (a grab of it %2 % changed in 1 s)")
                                    .arg(*frames).arg(all ? 100.0 * changed / all : 0, 0, 'f', 1));
        });
        at(13500, [=, &viewer] { viewer.zoomStep(2); });
        at(14500, [=, &viewer] {
            say(describe());
            QQuickItem *l = qobject_cast<QQuickItem *>(layer());
            const QRectF a = inScene(l), b = inScene(frameOf());
            check(viewer.zoom() > 1.5 && playing() && std::abs(a.x() - b.x()) < 0.5 && std::abs(a.width() - b.width()) < 0.5,
                  QStringLiteral("zoomed in %1×, the bounce plays on the photo as it lies").arg(viewer.zoom(), 0, 'f', 2));
            grab(QStringLiteral("live-bounce-zoomed"));
            viewer.resetZoom();
        });
        at(19500, [=] {
            say(describe());
            QObject *p = player();
            check(playing() && p && p->property("loops").toInt() < 0 && *wraps >= 1,
                  QStringLiteral("the bounce goes round and round: back to its start %1 times in 9 s").arg(*wraps));
            choose(QStringLiteral("long_exposure"));
            check(effect() == QLatin1String("long_exposure") && !playing(), QStringLiteral("Long exposure chosen: the bounce stops"));
        });
        at(25000, [=] {
            say(describe());
            check(layer() && !layer()->property("longExposure").toString().isEmpty(), QStringLiteral("the long exposure shows"));
            grab(QStringLiteral("live-long"));
            QMetaObject::invokeMethod(live(), "choose", Q_ARG(QVariant, QVariant(QStringLiteral("bounce"))));
        });
        at(28000, [=, &viewer] {
            say(describe());
            check(playing(), QStringLiteral("a bounce again, playing"));
            QPointer<QObject> was = layer();
            press(window, Qt::Key_Escape);
            check(was && !was->property("playing").toBool() && !was->property("shown").toBool(),
                  QStringLiteral("Esc: the motion stops as the photo shrinks"));
            QTimer::singleShot(60, window, [=] { grab(QStringLiteral("live-esc")); });
            Q_UNUSED(viewer);
        });
        at(29500, [=, &viewer] {
            check(!viewer.isOpen() && layer() == nullptr, QStringLiteral("closed: no layer left"));
            if (looped >= 0)
                grid->reveal(looped);
        });
        at(30500, [=, &viewer] {
            if (looped < 0)
                return;
            viewer.openAt(looped, grid->rectOf(looped));
            QTimer::singleShot(70, window, [=] { grab(QStringLiteral("live-grow-1")); });
            QTimer::singleShot(160, window, [=] { grab(QStringLiteral("live-grow-2")); });
        });
        at(33500, [=] {
            say(describe());
            if (looped < 0) {
                say(QStringLiteral("no loop set on the iPhone in the library"));
                return;
            }
            QObject *p = player();
            check(playing() && p && p->property("loops").toInt() < 0,
                  QStringLiteral("a loop set on the iPhone plays round and round as it shows"));
            grab(QStringLiteral("live-loop"));
            // Bounce chosen as the loop plays: the loop stops at once, and
            // the bounce plays once here; or, where it cannot be had
            // (offline, no motion), nothing plays under a plain button and
            // the button says so.
            if (QObject *l = layer())
                onChange(l, "failed", l, [=] {
                    if (layer() && layer()->property("failed").toString() == QLatin1String("bounce"))
                        QTimer::singleShot(150, window, [=] {
                            QQuickItem *b = button();
                            *bounceFailed = describe() + "; " + state();
                            *bounceFailedRight = !playing() && b && !b->property("active").toBool()
                                && complaint(window->contentItem()) == QLatin1String("Could not show this effect");
                            grab(QStringLiteral("live-bounce-failed"));
                        });
                });
            choose(QStringLiteral("bounce"));
            QObject *l = layer();
            check(effect() == QLatin1String("bounce") && l && !playing()
                      && l->property("sourceKind").toString() != QLatin1String("bounce"),
                  QStringLiteral("Bounce chosen as the loop plays: the loop stops before the bounce comes (%1)").arg(describe()));
        });
        at(41500, [=] {
            if (looped < 0)
                return;
            say(describe());
            QObject *l = layer();
            if (bounceFailed->isEmpty()) {
                check(l && playing() && l->property("sourceKind").toString() == QLatin1String("bounce"),
                      QStringLiteral("the bounce came, and plays in the loop's place"));
            } else {
                say(QStringLiteral("the bounce could not be had: ") + *bounceFailed);
                check(*bounceFailedRight, QStringLiteral("… so nothing plays, the button is not lit, and it says so"));
                check(l && !playing(), QStringLiteral("… and nothing plays later either"));
            }
            grab(QStringLiteral("live-bounce-after-loop"));
            press(window, Qt::Key_Escape);
        });
        at(43000, [=] {
            for (const char *kind : {"loop", "bounce", "live"})
                check(fetchedOk(QString::fromLatin1(kind)), QStringLiteral("Background has the %1").arg(QString::fromLatin1(kind)));
            done();
        });
    }

    QTimer::singleShot(2000, window, [window, steps] {
        for (const auto &[ms, f] : *steps)
            QTimer::singleShot(ms, window, f);
    });
}
