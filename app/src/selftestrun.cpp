// Which self-test PINHOLE_SELFTEST names, started on the window as it
// opens; those that are only pictures of the window are here, the rest in
// selftest.cpp.

#include "selftest.h"

#include "incoming.h"
#include "library.h"
#include "outgoing.h"
#include "photogrid.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

bool runSelfTest(const QString &test, const SelfTestContext &context) {
    // `name`, or `name:argument` (a folder or a file, mostly).
    const QString name = test.section(':', 0, 0), arg = test.section(':', 1);
    static const QStringList known = {"sourcesfade", "shot", "help", "selectbar", "settings", "sourcesbar", "zoomfast", "wheelzoom", "zoomdates", "neon", "drop", "viewer", "copytwice", "live", "actionbar", "layers", "favourite", "tour", "series", "undo", "theme", "background", "mouse", "keys", "covering", "inputs", "reload", "outgoing", "otherseries", "plugins", "sheet", "send", "frame", "hidden", "stacks", "sources", "sourcesoff", "through"};
    if (!known.contains(name)) {
        std::fprintf(stderr, "pinhole: no self-test %s (there are: %s)\n", qPrintable(name), qPrintable(known.join(", ")));
        return false;
    }
    QQuickWindow *window = context.window;
    // PINHOLE_SELFTEST_SIZE: the window's, as 1400x900, for any of them
    // (offscreen, or a window the compositor lets float): how it looks and
    // works that wide, the bars folding and the help in its columns.
    if (const QStringList size = qEnvironmentVariable("PINHOLE_SELFTEST_SIZE").split('x'); size.size() == 2)
        window->resize(size[0].toInt(), size[1].toInt());
    PhotoGrid *grid = context.grid;
    Library &library = context.library;
    Viewer &viewer = context.viewer;
    Incoming &incoming = context.incoming;
    Outgoing &outgoing = context.outgoing;
    Keymap &keymap = context.keymap;
    // PINHOLE_SELFTEST=sourcesfade:<dir>: the side panel coming in and going
    // again, the photos dimming behind it and the grid's controls fading, as
    // frames in <dir> (on screen, for the photos to be drawn).
    if (name == QLatin1String("sourcesfade")) {
        QTimer::singleShot(2500, window, [window, arg] {
            QObject *bar = window->findChild<QObject *>(QStringLiteral("sidePanel"));
            const QString dir = arg;
            auto grab = [window, dir](const QString &name) { window->grabWindow().save(dir + '/' + name + ".png"); };
            grab("0-before");
            // Apart, as a grab on screen comes back with a frame from a
            // moment later.
            QTimer::singleShot(500, window, [=] { bar->setProperty("shown", true); });
            QTimer::singleShot(560, window, [=] { grab("1-coming"); });
            QTimer::singleShot(1000, window, [=] {
                grab("2-shown");
                bar->setProperty("shown", false);
                QTimer::singleShot(80, window, [=] { grab("3-going"); });
                QTimer::singleShot(400, window, [=] {
                    grab("4-after");
                    QCoreApplication::quit();
                });
            });
        });
    }
    // PINHOLE_SELFTEST=help:<file>: the help as images, to check its layout
    // (and each language's) without a screen: <file> its keyboard tab, and
    // beside it <file>-mouse and <file>-touchpad. Shown after
    // PINHOLE_SELFTEST_DELAY ms (800): as the window opens, or once it has.
    // PINHOLE_SELFTEST=shot:<file>: the window as it first shows, as an image.
    if (name == QLatin1String("shot")) {
        // PINHOLE_SELFTEST_DELAY: how long to wait first, in ms.
        const int delay = qEnvironmentVariableIsSet("PINHOLE_SELFTEST_DELAY") ? qEnvironmentVariableIntValue("PINHOLE_SELFTEST_DELAY") : 1200;
        QTimer::singleShot(delay, window, [window, arg] {
            window->grabWindow().save(arg);
            QCoreApplication::quit();
        });
    }
    if (name == QLatin1String("help")) {
        const int delay = qEnvironmentVariableIsSet("PINHOLE_SELFTEST_DELAY") ? qEnvironmentVariableIntValue("PINHOLE_SELFTEST_DELAY") : 800;
        // Each tab as drawn: its parts apart, none over another, and every
        // text within its place (none running on into the next part).
        auto failures = std::make_shared<int>(0);
        auto apart = [window, failures](const QString &tab) {
            QQuickItem *board = window->findChild<QQuickItem *>(QStringLiteral("helpBoard"));
            QList<QRectF> parts;
            int wider = 0;
            std::function<void(QQuickItem *, QRectF &)> texts = [&](QQuickItem *item, QRectF &extent) {
                for (QQuickItem *child : item->childItems()) {
                    if (const QVariant content = child->property("contentWidth"); content.isValid() && child->property("text").isValid()) {
                        extent |= child->mapRectToItem(board, QRectF(0, 0, child->width(), child->height()));
                        wider += content.toReal() > child->width() + 1;
                    }
                    texts(child, extent);
                }
            };
            for (QQuickItem *part : board ? board->childItems() : QList<QQuickItem *>()) {
                QRectF extent;
                texts(part, extent);
                if (!extent.isEmpty())
                    parts.append(extent);
            }
            int over = 0;
            for (int i = 0; i < parts.size(); ++i)
                for (int j = i + 1; j < parts.size(); ++j)
                    over += parts[i].intersects(parts[j]);
            const bool ok = !parts.isEmpty() && over == 0 && wider == 0;
            std::printf("selftest: %s the %s tab's %lld parts apart (%d over another, %d texts wider than their place)\n",
                        ok ? "ok  " : "FAIL", qPrintable(tab), static_cast<long long>(parts.size()), over, wider);
            std::fflush(stdout);
            if (!ok)
                ++*failures;
        };
        QTimer::singleShot(delay, window, [window, arg, apart, failures] {
            if (QObject *help = window->findChild<QObject *>(QStringLiteral("help")))
                help->setProperty("shown", true);
            QTimer::singleShot(400, window, [window, arg, apart, failures] {
                QObject *help = window->findChild<QObject *>(QStringLiteral("help"));
                const QString file = arg;
                const QString stem = file.left(file.lastIndexOf('.')), suffix = file.mid(file.lastIndexOf('.'));
                help->setProperty("tab", QStringLiteral("keyboard"));
                QTimer::singleShot(200, window, [=] {
                    window->grabWindow().save(file);
                    apart("keyboard");
                    help->setProperty("tab", QStringLiteral("mouse"));
                    QTimer::singleShot(200, window, [=] {
                        window->grabWindow().save(stem + "-mouse" + suffix);
                        apart("mouse");
                        help->setProperty("tab", QStringLiteral("touchpad"));
                        QTimer::singleShot(200, window, [=] {
                            window->grabWindow().save(stem + "-touchpad" + suffix);
                            apart("touchpad");
                            QCoreApplication::exit(*failures ? 1 : 0);
                        });
                    });
                });
            });
        });
    }
    // PINHOLE_SELFTEST=selectbar:<dir>: select mode's bar with three photos
    // ticked, at window widths from wide to a phone's, as images in <dir>.
    if (name == QLatin1String("selectbar")) {
        QTimer::singleShot(1200, window, [window, grid, arg] {
            grid->setSelecting(true);
            grid->selectAll();
            grid->jumpToFraction(0.5); // away from the newest: the Newest button shows
            auto widths = std::make_shared<QList<int>>(QList<int>{1400, 1000, 760, 560, 420, 360});
            auto next = std::make_shared<std::function<void()>>();
            *next = [window, grid, arg, widths, next] {
                if (widths->isEmpty()) {
                    QCoreApplication::quit();
                    return;
                }
                const int width = widths->takeFirst();
                window->resize(width, 600);
                QTimer::singleShot(300, window, [window, arg, width, next] {
                    window->grabWindow().copy(0, 440, width, 160).save(QStringLiteral("%1/bar-%2.png").arg(arg).arg(width));
                    (*next)();
                });
            };
            (*next)();
        });
    }
    // PINHOLE_SELFTEST=settings:<dir>: the side panel as it opens, as an
    // image in <dir>, a page at a time, then each mark's switch pressed
    // twice, so left as it was, and the last plugin with marks turned off
    // and on again (exit 1 if a switch does not turn its mark off and on
    // in the grid, or the plugin's marks do not go and come back with it;
    // its switch is kept, so on a scratch config only). With
    // PINHOLE_SELFTEST_LOWER=1 also with the limit
    // stepped two down, and once that is confirmed: it removes photos, so
    // only for a service on a scratch cache.
    if (name == QLatin1String("settings")) {
        QTimer::singleShot(1200, window, [window, arg] {
            QObject *panel = window->findChild<QObject *>(QStringLiteral("sidePanel"));
            const QString dir = arg;
            panel->setProperty("shown", true);
            QTimer::singleShot(1000, window, [=] {
                window->grabWindow().save(dir + "/1-open.png");
                if (qEnvironmentVariable("PINHOLE_SELFTEST_LOWER") != QLatin1String("1")) {
                    // Then a page further down at a time, to its end, as
                    // 1-open-2.png and on: the plugins, a dozen of them.
                    QObject *scroller = window->findChild<QObject *>(QStringLiteral("sidePanelScroller"));
                    auto page = std::make_shared<std::function<void(int)>>();
                    *page = [=](int n) {
                        const double height = scroller->property("height").toDouble();
                        const double most = scroller->property("contentHeight").toDouble() - height;
                        const double y = scroller->property("contentY").toDouble();
                        if (y >= most - 1 || n > 12) {
                            // Then each mark's switch, pressed and pressed
                            // again: the grid draws it or not, and the switch
                            // says so.
                            PhotoGrid *grid = window->findChild<PhotoGrid *>(QStringLiteral("grid"));
                            int failures = 0;
                            // A Repeater's rows are the panel's items, not
                            // its QObject children.
                            QList<QQuickItem *> items;
                            std::function<void(QQuickItem *)> gather = [&](QQuickItem *item) {
                                items << item;
                                for (QQuickItem *child : item->childItems())
                                    gather(child);
                            };
                            gather(window->contentItem());
                            for (QQuickItem *toggle : items) {
                                const QString id = toggle->objectName().section(':', 1);
                                if (!toggle->objectName().startsWith(QLatin1String("markSwitch:")))
                                    continue;
                                bool ok = true;
                                for (int press = 0; press < 2; ++press) {
                                    const bool was = toggle->property("checked").toBool();
                                    ok &= was == grid->marksShown().contains(id);
                                    QMetaObject::invokeMethod(toggle, "press");
                                    ok &= toggle->property("checked").toBool() == !was && grid->marksShown().contains(id) == !was;
                                }
                                std::printf("selftest: %s the switch of %s turns it %s and back\n", ok ? "ok  " : "FAIL",
                                            qPrintable(id), grid->marksShown().contains(id) ? "off" : "on");
                                failures += !ok;
                            }
                            std::fflush(stdout);
                            // And a plugin's marks go with it turned off, and
                            // come back with it turned on.
                            QObject *plugins = qmlEngine(window)->rootContext()->contextProperty("plugins").value<QObject *>();
                            auto marksOf = [grid](const QString &plugin) {
                                QStringList ids;
                                for (const QVariant &kind : grid->markKinds())
                                    if (const QString id = kind.toMap().value("id").toString(); id.startsWith(plugin + '/'))
                                        ids << id;
                                return ids;
                            };
                            QString marking;
                            for (const QVariant &kind : plugins->property("markKinds").toList())
                                if (const QString plugin = kind.toMap().value("plugin").toString(); !plugin.isEmpty())
                                    marking = plugin;
                            if (marking.isEmpty()) {
                                QCoreApplication::exit(failures ? 1 : 0);
                                return;
                            }
                            const QStringList had = marksOf(marking);
                            QMetaObject::invokeMethod(plugins, "setEnabled", Q_ARG(QString, marking), Q_ARG(bool, false));
                            QTimer::singleShot(1000, window, [=] {
                                const QStringList off = marksOf(marking);
                                std::printf("selftest: %s %s turned off takes its marks (%s) with it: %s left\n",
                                            off.isEmpty() ? "ok  " : "FAIL", qPrintable(marking), qPrintable(had.join(", ")),
                                            off.isEmpty() ? "none" : qPrintable(off.join(", ")));
                                QMetaObject::invokeMethod(plugins, "setEnabled", Q_ARG(QString, marking), Q_ARG(bool, true));
                                QTimer::singleShot(2000, window, [=] {
                                    const QStringList on = marksOf(marking);
                                    std::printf("selftest: %s turned on again, its marks are back: %s\n", on == had ? "ok  " : "FAIL",
                                                qPrintable(on.join(", ")));
                                    std::fflush(stdout);
                                    QCoreApplication::exit(failures || !off.isEmpty() || on != had ? 1 : 0);
                                });
                            });
                            return;
                        }
                        scroller->setProperty("contentY", std::min(most, y + height * 0.9));
                        QTimer::singleShot(300, window, [=] {
                            window->grabWindow().save(dir + QStringLiteral("/1-open-%1.png").arg(n));
                            (*page)(n + 1);
                        });
                    };
                    (*page)(2);
                    return;
                }
                QMetaObject::invokeMethod(panel, "step", Q_ARG(QVariant, -1));
                QMetaObject::invokeMethod(panel, "step", Q_ARG(QVariant, -1));
                QTimer::singleShot(800, window, [=] {
                    window->grabWindow().save(dir + "/2-lower.png");
                    QMetaObject::invokeMethod(panel, "confirmLimit");
                    QTimer::singleShot(2500, window, [=] {
                        window->grabWindow().save(dir + "/3-freed.png");
                        QCoreApplication::quit();
                    });
                });
            });
        });
    }
    // PINHOLE_SELFTEST=plugins:<dir>: the side panel's plugins, the one
    // PINHOLE_SELFTEST_PLUGIN names (Series, else) in view, as an image in
    // <dir>: what each says of itself and what went wrong in it
    // (PINHOLE_BUILTIN_PLUGINS for some that go wrong).
    if (name == QLatin1String("plugins")) {
        QTimer::singleShot(2500, window, [window, arg] {
            QObject *panel = window->findChild<QObject *>(QStringLiteral("sidePanel"));
            panel->setProperty("shown", true);
            QTimer::singleShot(800, window, [=] {
                const QString id = qEnvironmentVariable("PINHOLE_SELFTEST_PLUGIN", QStringLiteral("pinhole.series"));
                QVariant row;
                QMetaObject::invokeMethod(panel, "showPlugin", Q_RETURN_ARG(QVariant, row), Q_ARG(QVariant, id));
                QTimer::singleShot(1500, window, [=] {
                    window->grabWindow().save(arg + "/plugins.png");
                    // What the panel says of each, as Plugins::at has it.
                    if (QObject *plugins = qvariant_cast<QObject *>(QQmlEngine::contextForObject(panel)->contextProperty("plugins")))
                        for (const QString &each : plugins->property("ids").toStringList()) {
                            QVariantMap info;
                            QMetaObject::invokeMethod(plugins, "at", Q_RETURN_ARG(QVariantMap, info), Q_ARG(QString, each));
                            const QString problem = info.value("error").toString() + info.value("problem").toString();
                            std::printf("selftest: %s: %s\n", qPrintable(each), problem.isEmpty() ? "fine" : qPrintable(problem));
                        }
                    std::fflush(stdout);
                    QCoreApplication::quit();
                });
            });
        });
    }
    // PINHOLE_SELFTEST=sourcesbar:<dir>: the side panel as it opens and a
    // few seconds and a round later, as images in <dir>, to see when the
    // source shown was looked at count on.
    if (name == QLatin1String("sourcesbar")) {
        QTimer::singleShot(1500, window, [window, arg] {
            window->findChild<QObject *>(QStringLiteral("sidePanel"))->setProperty("shown", true);
            const QString dir = arg;
            for (const int seconds : {1, 4, 13}) {
                QTimer::singleShot(seconds * 1000, window, [=] {
                    window->grabWindow().save(dir + QStringLiteral("/after-%1s.png").arg(seconds));
                    if (seconds == 13)
                        QCoreApplication::quit();
                });
            }
        });
    }
    // PINHOLE_SELFTEST=zoomfast:<dir>: a quick pinch from 3 columns all the
    // way out, the date label's state every 16 ms on the output and as
    // frames of the top left in <dir>.
    if (name == QLatin1String("zoomfast")) {
        QTimer::singleShot(1200, window, [window, grid, arg] {
            grid->setColumnsNow(3);
            grid->jumpToFraction(0.6);
            auto *clock = new QElapsedTimer;
            auto *frame = new int(0);
            auto *timer = new QTimer(window);
            QObject *label = window->findChild<QObject *>(QStringLiteral("dateLabel"));
            QTimer::singleShot(500, window, [=] {
                clock->start();
                QObject::connect(timer, &QTimer::timeout, window, [=] {
                    const double t = clock->nsecsElapsed() / 1e9;
                    const QPointF middle(grid->width() / 2, grid->height() / 2);
                    if (t < 0.5)
                        grid->setEffectiveStride(grid->width() / (3 * std::pow(29.0 / 3, t / 0.5)), middle);
                    else if (!grid->property("ended").toBool()) {
                        grid->setProperty("ended", true);
                        grid->endContinuousZoom();
                    } else if (t > 1.5) {
                        timer->stop();
                        QCoreApplication::quit();
                    }
                    std::printf("%03d %5.3f grid=%-28s shown=%-28s morphed=%.2f ticking=%d w=%.1f\n", *frame, t,
                                qPrintable(grid->dateLabel()), qPrintable(label->property("shownText").toString()),
                                label->property("morphed").toDouble(), label->property("ticking").toBool(),
                                label->property("width").toDouble());
                    std::fflush(stdout);
                    window->grabWindow().copy(0, 0, 420, 70).save(QStringLiteral("%1/%2.png").arg(arg).arg(*frame, 3, 10, QLatin1Char('0')));
                    ++*frame;
                });
                timer->start(16);
            });
        });
    }
    // PINHOLE_SELFTEST=wheelzoom: a mouse's horizontal wheel turned a little
    // at a time from 7 columns, a second apart, and the level each turn lands
    // on, on the output: never back where it began unless it hardly turned,
    // and never more than a level, even for a burst of notches (as an MX
    // Master's thumb wheel sends for the slightest touch).
    if (name == QLatin1String("wheelzoom")) {
        QTimer::singleShot(1200, window, [window, grid] {
            grid->setColumnsNow(7);
            grid->jumpToFraction(0.5);
            // Frames stepped here too, in case the window is hidden and draws none.
            auto *frames = new QTimer(window);
            QObject::connect(frames, &QTimer::timeout, window, [grid] { grid->advance(1.0 / 60); });
            frames->start(16);
            const std::vector<std::vector<int>> turns = {{24}, {120}, {-120}, {-120}, {-120}, {120, 120, 120}, {60}, {-24},
                                                       {-120, -120, -120, -120, -120, -120, -120, -120}, {360, 360}};
            for (size_t k = 0; k <= turns.size(); ++k) {
                QTimer::singleShot(int(k) * 1000 + 500, window, [grid, turns, k] {
                    if (k > 0)
                        std::printf("turn %zu: lands on %d columns\n", k, grid->columns()), std::fflush(stdout);
                    if (k == turns.size())
                        return QCoreApplication::quit();
                    for (size_t n = 0; n < turns[k].size(); ++n)
                        QTimer::singleShot(int(n) * 30, grid, [grid, x = turns[k][n]] {
                            const QPointF at(grid->width() / 2, grid->height() / 2);
                            QWheelEvent event(at, grid->mapToGlobal(at), QPoint(), QPoint(x, 0), Qt::NoButton, Qt::NoModifier,
                                              Qt::NoScrollPhase, false);
                            QCoreApplication::sendEvent(grid, &event);
                        });
                });
            }
        });
    }
    // PINHOLE_SELFTEST=zoomdates:<dir>: the date label through a pinch from
    // 7 columns out to 29 and in to 3, then two seconds of scrolling at 7
    // (where the label is a range of days), as frames of the top left in
    // <dir>, with the label of each on the output.
    if (name == QLatin1String("zoomdates")) {
        QTimer::singleShot(1200, window, [window, grid, arg] {
            grid->setColumnsNow(7);
            grid->jumpToFraction(0.5);
            auto *timer = new QTimer(window);
            auto *frame = new int(0);
            QObject::connect(timer, &QTimer::timeout, window, [window, grid, arg, timer, frame] {
                const double t = *frame * 0.05;
                auto lerpLog = [](double a, double b, double f) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * f); };
                if (t < 2.0)
                    grid->setEffectiveStride(grid->width() / lerpLog(7, 29, t / 2.0), QPointF(grid->width() / 2, grid->height() / 2));
                else if (t < 4.0)
                    grid->setEffectiveStride(grid->width() / lerpLog(29, 3, (t - 2.0) / 2.0), QPointF(grid->width() / 2, grid->height() / 2));
                else if (t < 4.05)
                    grid->endContinuousZoom();
                // Then scrolling up through the days, the label changing as
                // it goes.
                else if (t >= 4.6 && t < 4.65)
                    grid->setColumnsNow(7), grid->jumpToFraction(0.97), grid->setDriveVelocity(-6000);
                else if (t >= 6.6 && t < 6.65)
                    grid->setDriveVelocity(0);
                else if (t > 7.0) {
                    timer->stop();
                    QCoreApplication::quit();
                }
                std::printf("%03d %s %.4f %s\n", *frame, grid->zooming() ? "zooming" : "still  ", grid->scrollFraction(),
                            grid->dateLabel().isEmpty() ? "-" : qPrintable(grid->dateLabel()));
                std::fflush(stdout);
                window->grabWindow().copy(0, 0, 420, 70).save(QStringLiteral("%1/%2.png").arg(arg).arg(*frame, 3, 10, QLatin1Char('0')));
                ++*frame;
            });
            timer->start(50);
        });
    }
    // PINHOLE_SELFTEST=neon:<dir>: zooming out to 29 columns, where the
    // marks' tubes strike, in and out again, then in to 7, where they are
    // white, and out to 13, where the photos dim and the tubes strike anew,
    // as 60 frames a second in <dir>
    // (PINHOLE_SELFTEST_AT: where in the library). Frames are stepped here,
    // not by the clock, so slow grabs do not change the timing. 29 first, so
    // its thumbnails are in.
    if (name == QLatin1String("neon")) {
        QTimer::singleShot(2500, window, [window, grid, arg] {
            const double at = qEnvironmentVariable("PINHOLE_SELFTEST_AT").toDouble();
            window->setProperty("troubleSeen", true);
            grid->setColumnsNow(29);
            grid->jumpToFraction(at);
            QTimer::singleShot(5000, window, [grid, at] { grid->setColumnsNow(13), grid->jumpToFraction(at); });
            QTimer::singleShot(8000, window, [window, grid, arg] {
                int frame = 0;
                auto frames = [&](int n) {
                    for (int i = 0; i < n; ++i, ++frame) {
                        grid->advance(1.0 / 60);
                        window->grabWindow().save(QStringLiteral("%1/%2.jpg").arg(arg).arg(frame, 4, 10, QLatin1Char('0')), nullptr, 92);
                    }
                };
                frames(30);
                grid->zoomStep(1);
                frames(150);
                grid->zoomStep(-1);
                frames(50);
                grid->zoomStep(1);
                frames(150);
                grid->zoomStep(-1);
                frames(50);
                grid->zoomStep(-1);
                frames(50);
                grid->zoomStep(1);
                frames(150);
                QCoreApplication::quit();
            });
        });
    }
    if (name == QLatin1String("drop"))
        runDropSelfTest(window, library, incoming, arg, qEnvironmentVariable("PINHOLE_SELFTEST_DROP").split(':', Qt::SkipEmptyParts));
    if (name == QLatin1String("viewer"))
        runViewerSelfTest(window, library, viewer);
    if (name == QLatin1String("copytwice"))
        runCopyTwiceSelfTest(window, grid, viewer, arg);
    if (name == QLatin1String("live"))
        runLiveSelfTest(window, grid, library, viewer, arg);
    if (name == QLatin1String("actionbar"))
        runActionBarSelfTest(window, grid, viewer, library, arg);
    if (name == QLatin1String("layers"))
        runLayersSelfTest(window, grid, viewer, library, arg);
    if (name == QLatin1String("sources"))
        runSourcesSelfTest(window, arg);
    if (name == QLatin1String("sourcesoff"))
        runSourcesOffSelfTest(window, arg);
    if (name == QLatin1String("favourite"))
        runFavouriteSelfTest(window, grid, viewer, library, arg);
    if (name == QLatin1String("tour"))
        runTourSelfTest(window, viewer, arg);
    if (name == QLatin1String("series"))
        runSeriesSelfTest(window, viewer, arg);
    if (name == QLatin1String("otherseries"))
        runOtherSeriesSelfTest(window, library, context.reloader, arg);
    if (name == QLatin1String("sheet"))
        runSheetSelfTest(window, grid, arg);
    if (name == QLatin1String("frame"))
        runFrameSelfTest(window, library);
    if (name == QLatin1String("hidden"))
        runHiddenSelfTest(window, grid, library, viewer, arg);
    if (name == QLatin1String("stacks"))
        runStacksSelfTest(window, grid, library, context.reloader);
    if (name == QLatin1String("undo"))
        runUndoSelfTest(window, viewer, arg);
    if (name == QLatin1String("theme"))
        runThemeSelfTest(window, library, viewer, arg);
    if (name == QLatin1String("background"))
        runBackgroundSelfTest(window, library, viewer, arg);
    if (name == QLatin1String("mouse"))
        runMouseSelfTest(window, grid, viewer, library);
    if (name == QLatin1String("keys"))
        runKeysSelfTest(window, grid, viewer, keymap);
    if (name == QLatin1String("through"))
        runThroughSelfTest(window, grid, viewer, arg);
    if (name == QLatin1String("covering"))
        runCoveringSelfTest(window, viewer, keymap);
    if (name == QLatin1String("inputs"))
        runInputsSelfTest(window, grid, arg);
    if (name == QLatin1String("reload"))
        runReloadSelfTest(library, grid, viewer);
    if (name == QLatin1String("outgoing"))
        runOutgoingSelfTest(library, outgoing, qEnvironmentVariable("PINHOLE_SELFTEST_DIR"));
    // PINHOLE_SELFTEST=send:<dir>: Send to phone on a Live Photo and a
    // HEIC, as O over the viewer does; grabbed into <dir> as the devices
    // answer, then sent to the first, as 1 does, and grabbed when it is
    // through. A LocalSend device on the network to take it (a phone with
    // LocalSend open, or the plugin's test receiver) is the run's. With
    // PINHOLE_SELFTEST_SEND=stop, Stop is pressed while the device decides;
    // =pin, the PIN it asks for is 1234, asked while closed, then typed with
    // Pinhole's keys checked around it (exit 1 if any fails); =closed, the
    // panel is closed as it
    // goes, and how it ended shows at the foot of the window; =stopsending,
    // Stop is pressed 2.5 s into the send (the run's device having stopped
    // answering); =localsend, L in place of a device; =jpeg, HEIC as JPEG.
    if (name == QLatin1String("send")) {
        QTimer::singleShot(3000, window, [window, &library, &viewer, arg] {
            auto say = [](const QString &line) {
                std::printf("selftest: %s\n", qPrintable(line));
                std::fflush(stdout);
            };
            auto grab = [window, arg](const QString &name) { window->grabWindow().save(arg + '/' + name + ".png"); };
            std::function<QQuickItem *(QQuickItem *)> find = [&find](QQuickItem *item) -> QQuickItem * {
                if (item->objectName() == QLatin1String("sendPanel"))
                    return item;
                for (QQuickItem *child : item->childItems())
                    if (QQuickItem *found = find(child))
                        return found;
                return nullptr;
            };
            QQuickItem *panel = find(window->contentItem());
            if (!panel) {
                say(QStringLiteral("no Send to phone panel"));
                return QCoreApplication::quit();
            }
            int live = -1, heic = -1;
            for (int i = library.count() - 1; i >= 0 && (live < 0 || heic < 0); --i) {
                const LibraryItem &item = library.at(i);
                if (item.live && live < 0)
                    live = i;
                else if (!item.video && !item.live && item.filename.endsWith(QLatin1String(".HEIC"), Qt::CaseInsensitive) && heic < 0)
                    heic = i;
            }
            say(QStringLiteral("a Live Photo at %1, a HEIC at %2").arg(live).arg(heic));
            viewer.openAt(live >= 0 ? live : heic, QRectF(0, 0, 100, 100));
            QVariantList indexes;
            for (int i : {live, heic})
                if (i >= 0)
                    indexes.append(i);
            QTimer::singleShot(1500, window, [=] {
                QMetaObject::invokeMethod(panel, "open", Q_ARG(QVariant, QVariant(indexes)));
                QTimer::singleShot(400, window, [=] { grab(QStringLiteral("1-open")); });
            });
            const QString mode = qEnvironmentVariable("PINHOLE_SELFTEST_SEND");
            if (mode == QLatin1String("jpeg"))
                QMetaObject::invokeMethod(panel->property("plugin").value<QObject *>(), "setCompatible", Q_ARG(QVariant, true));
            auto describe = [panel] {
                return QStringLiteral("%1 devices, %2 files (%5 fetched), phase \"%3\", %4")
                    .arg(panel->property("devices").toList().size())
                    .arg(panel->property("files").toList().size())
                    .arg(panel->property("phase").toString(), panel->property("status").toString())
                    .arg(panel->property("fetched").toInt());
            };
            Viewer *photos = &viewer;
            QTimer::singleShot(9000, window, [=] {
                grab(QStringLiteral("2-devices"));
                say(describe());
                const QVariantList devices = panel->property("devices").toList();
                if (mode == QLatin1String("localsend"))
                    QMetaObject::invokeMethod(panel, "openLocalSend");
                else if (devices.isEmpty())
                    return QCoreApplication::quit();
                else
                    QMetaObject::invokeMethod(panel, "sendTo", Q_ARG(QVariant, devices.first()), Q_ARG(QVariant, QString()));
                QTimer::singleShot(300, window, [=] { grab(QStringLiteral("3-asking")); });
                if (mode == QLatin1String("stop"))
                    QTimer::singleShot(500, window, [=] { QMetaObject::invokeMethod(panel, "stopSending"); });
                // =pin: closed too, so the PIN is asked for while it is.
                if (mode == QLatin1String("closed") || mode == QLatin1String("pin"))
                    QTimer::singleShot(100, window, [=] { QMetaObject::invokeMethod(panel, "close"); });
                auto clock = std::make_shared<QElapsedTimer>();
                clock->start();
                auto stopping = std::make_shared<bool>(false);
                // =pin: typed as a person does, and Pinhole's keys checked
                // around it: O opens the panel again on the prompt, the PIN
                // is typed and the panel closed with Esc, → goes to the next
                // photo, O again, the PIN still there, ⏎ sends it.
                auto typing = std::make_shared<bool>(false);
                auto failures = std::make_shared<int>(0);
                auto field = panel->findChild<QQuickItem *>(QStringLiteral("send.pin"));
                auto press = [window](int code, const QString &text = {}) {
                    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier, text);
                    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier, text);
                };
                auto check = [say, failures](bool ok, const QString &what) {
                    *failures += ok ? 0 : 1;
                    say(QStringLiteral("%1 %2").arg(ok ? "ok  " : "FAIL", what));
                };
                auto shown = [panel] { return panel->property("shown").toBool(); };
                auto focused = [field] { return field && field->hasActiveFocus(); };
                auto at = std::make_shared<int>(-1);
                // ← to the photo before (the one shown is the newest), and →
                // back to it; then `then`.
                auto arrows = [=](const std::function<void()> &then) {
                    *at = photos->index();
                    press(Qt::Key_Left);
                    QTimer::singleShot(600, window, [=] {
                        const int before = photos->index();
                        check(before != *at, QStringLiteral("← goes to the photo before (%1 to %2)").arg(*at).arg(before));
                        press(Qt::Key_Right);
                        QTimer::singleShot(600, window, [=] {
                            check(photos->index() == *at, QStringLiteral("→ comes back (%1 to %2)").arg(before).arg(photos->index()));
                            then();
                        });
                    });
                };
                auto pinKeys = [=] {
                    grab(QStringLiteral("4-pin-closed"));
                    say(describe());
                    check(!shown() && !focused(), QStringLiteral("asked while closed, the field has not the keys"));
                    press(Qt::Key_O, QStringLiteral("o"));
                    QTimer::singleShot(700, window, [=] {
                        grab(QStringLiteral("4-pin"));
                        check(shown() && panel->property("phase").toString() == QLatin1String("pin") && focused(),
                              QStringLiteral("O opens it on the prompt, the field with the keys"));
                        for (const char *digit : {"1", "2", "3", "4"})
                            press(Qt::Key_0 + (digit[0] - '0'), QString::fromLatin1(digit));
                        check(field && field->property("text").toString() == QLatin1String("1234"), QStringLiteral("the PIN typed"));
                        press(Qt::Key_Escape);
                        QTimer::singleShot(500, window, [=] {
                            check(!shown() && !focused(), QStringLiteral("Esc closes it, and the field lets go of the keys"));
                            arrows([=] {
                                press(Qt::Key_O, QStringLiteral("o"));
                                QTimer::singleShot(700, window, [=] {
                                    check(shown() && focused() && field->property("text").toString() == QLatin1String("1234"),
                                          QStringLiteral("O opens it again, the PIN still in its field"));
                                    *typing = false;
                                    press(Qt::Key_Return, QStringLiteral("\r"));
                                });
                            });
                        });
                    });
                };
                auto poll = new QTimer(window);
                poll->setInterval(200);
                QObject::connect(poll, &QTimer::timeout, window, [=] {
                    const QString phase = panel->property("phase").toString();
                    if (phase == QLatin1String("sending"))
                        grab(QStringLiteral("4-sending"));
                    if (phase == QLatin1String("sending") && mode == QLatin1String("stopsending") && !*stopping) {
                        *stopping = true;
                        QTimer::singleShot(2500, window, [=] {
                            say(QStringLiteral("Stop pressed: ") + describe());
                            QMetaObject::invokeMethod(panel, "stopSending");
                        });
                    }
                    if (mode == QLatin1String("pin") && (*typing || phase == QLatin1String("pin")) && clock->elapsed() < 60000) {
                        if (!*typing && *at < 0) {
                            *typing = true;
                            pinKeys();
                        }
                        return;
                    }
                    if (phase == QLatin1String("fetching") || phase == QLatin1String("asking") || phase == QLatin1String("sending")) {
                        if (clock->elapsed() < 60000)
                            return;
                    }
                    poll->stop();
                    grab(QStringLiteral("5-end"));
                    say(describe());
                    QTimer::singleShot(2500, window, [=] {
                        grab(QStringLiteral("6-later"));
                        say(QStringLiteral("panel %1").arg(shown() ? "still shown" : "closed"));
                        if (mode != QLatin1String("pin"))
                            return QCoreApplication::quit();
                        // Sent with the PIN: Pinhole's keys are its own.
                        check(phase == QLatin1String("sent"), QStringLiteral("sent with the PIN typed"));
                        check(!focused(), QStringLiteral("the field has not the keys"));
                        arrows([=] {
                            press(Qt::Key_O, QStringLiteral("o"));
                            QTimer::singleShot(700, window, [=] {
                                check(shown(), QStringLiteral("O opens it"));
                                say(QStringLiteral("pin: %1 failed").arg(*failures));
                                QCoreApplication::exit(*failures ? 1 : 0);
                            });
                        });
                    });
                });
                poll->start();
            });
        });
    }
    return true;
}
