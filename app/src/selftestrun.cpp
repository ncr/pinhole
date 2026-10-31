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
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

bool runSelfTest(const QString &test, const SelfTestContext &context) {
    // `name`, or `name:argument` (a folder or a file, mostly).
    const QString name = test.section(':', 0, 0), arg = test.section(':', 1);
    static const QStringList known = {"sourcesfade", "shot", "help", "selectbar", "settings", "sourcesbar", "zoomfast", "wheelzoom", "zoomdates", "neon", "drop", "viewer", "copytwice", "live", "actionbar", "favourite", "tour", "series", "undo", "mouse", "keys", "inputs", "reload", "outgoing", "otherseries"};
    if (!known.contains(name)) {
        std::fprintf(stderr, "pinhole: no self-test %s (there are: %s)\n", qPrintable(name), qPrintable(known.join(", ")));
        return false;
    }
    QQuickWindow *window = context.window;
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
    // beside it <file>-mouse and <file>-touchpad.
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
        // PINHOLE_SELFTEST_SIZE: the window's, as 1400x900.
        if (const QStringList size = qEnvironmentVariable("PINHOLE_SELFTEST_SIZE").split('x'); size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        QTimer::singleShot(800, window, [window, arg] {
            if (QObject *help = window->findChild<QObject *>(QStringLiteral("help")))
                help->setProperty("shown", true);
            QTimer::singleShot(400, window, [window, arg] {
                QObject *help = window->findChild<QObject *>(QStringLiteral("help"));
                const QString file = arg;
                const QString stem = file.left(file.lastIndexOf('.')), suffix = file.mid(file.lastIndexOf('.'));
                help->setProperty("tab", QStringLiteral("keyboard"));
                QTimer::singleShot(200, window, [=] {
                    window->grabWindow().save(file);
                    help->setProperty("tab", QStringLiteral("mouse"));
                    QTimer::singleShot(200, window, [=] {
                        window->grabWindow().save(stem + "-mouse" + suffix);
                        help->setProperty("tab", QStringLiteral("touchpad"));
                        QTimer::singleShot(200, window, [=] {
                            window->grabWindow().save(stem + "-touchpad" + suffix);
                            QCoreApplication::quit();
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
    // image in <dir>. With PINHOLE_SELFTEST_LOWER=1 also with the limit
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
                    QCoreApplication::quit();
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
        runViewerSelfTest(library, viewer);
    if (name == QLatin1String("copytwice"))
        runCopyTwiceSelfTest(window, grid, viewer, arg);
    if (name == QLatin1String("live"))
        runLiveSelfTest(window, library, viewer, arg);
    if (name == QLatin1String("actionbar"))
        runActionBarSelfTest(window, grid, viewer, library, arg);
    if (name == QLatin1String("favourite"))
        runFavouriteSelfTest(window, grid, viewer, library, arg);
    if (name == QLatin1String("tour"))
        runTourSelfTest(window, viewer, arg);
    if (name == QLatin1String("series"))
        runSeriesSelfTest(window, viewer, arg);
    if (name == QLatin1String("otherseries"))
        runOtherSeriesSelfTest(window, library, context.reloader, arg);
    if (name == QLatin1String("undo"))
        runUndoSelfTest(window, viewer, arg);
    if (name == QLatin1String("mouse"))
        runMouseSelfTest(window, grid, viewer, library);
    if (name == QLatin1String("keys"))
        runKeysSelfTest(window, grid, viewer, keymap);
    if (name == QLatin1String("inputs"))
        runInputsSelfTest(window, grid, arg);
    if (name == QLatin1String("reload"))
        runReloadSelfTest(library, grid, viewer);
    if (name == QLatin1String("outgoing"))
        runOutgoingSelfTest(library, outgoing, qEnvironmentVariable("PINHOLE_SELFTEST_DIR"));
    return true;
}
