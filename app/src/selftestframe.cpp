// PINHOLE_SELFTEST=frame: the Photo frame plugin as it runs (selftest.h).

#include "selftest.h"

#include "library.h"
#include "plugins.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVector3D>

#include <cstdio>
#include <functional>
#include <memory>

void runFrameSelfTest(QQuickWindow *window, const Library &library) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        std::printf("selftest: %s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
        std::fflush(stdout);
        if (!ok)
            ++*failures;
    };
    auto finish = [failures] { QCoreApplication::exit(*failures ? 1 : 0); };
    // `ready` asked every 20 ms, then `then`; or after `ms`, a failure.
    auto waitFor = std::make_shared<std::function<void(std::function<bool()>, int, QString, std::function<void()>)>>();
    *waitFor = [window, check, finish](std::function<bool()> ready, int ms, QString what, std::function<void()> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto *timer = new QTimer(window);
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, window, [=] {
            if (ready()) {
                timer->deleteLater();
                then();
            } else if (clock->elapsed() > ms) {
                timer->deleteLater();
                check(false, what + " (timed out)");
                finish();
            }
        });
        timer->start();
    };

    QTimer::singleShot(2500, window, [=, &library] {
        // The full Frame is created on first use, through the plugin's
        // action entry, before examining its slides here.
        QVariant instance;
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, instance),
                                  Q_ARG(QVariant, QStringLiteral("pinhole.frame")));
        if (auto *plugin = instance.value<QObject *>()) QMetaObject::invokeMethod(plugin, "ensureFrame");
        // A plugin's items are the window's items' children.
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("photoFrame"))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(child))
                    return found;
            return nullptr;
        };
        QQuickItem *frame = find(window->contentItem());
        auto *plugins = qobject_cast<Plugins *>(qmlEngine(window)->rootContext()->contextProperty("plugins").value<QObject *>());
        if (!frame || !plugins) {
            check(false, "the Photo frame plugin is loaded");
            return finish();
        }
        KeepAwake *awake = plugins->keepAwake();
        const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
        auto slide = [frame](const char *which) { return frame->property(which).value<QObject *>(); };

        // Each slide's medium and full size, looked at every 15 ms: while
        // the full size fades in, the medium is still under it (not the
        // thumbnail, a blur), and once it is in, the medium is let go.
        struct Fades {
            int midway = 0, bare = 0, dropped = 0;
        };
        auto fades = std::make_shared<Fades>();
        auto *watch = new QTimer(window);
        watch->setInterval(15);
        QObject::connect(watch, &QTimer::timeout, window, [=] {
            for (const char *which : {"current", "next"}) {
                QObject *s = slide(which);
                QObject *sharp = s ? s->findChild<QObject *>(QStringLiteral("sharpImage")) : nullptr;
                QObject *full = s ? s->findChild<QObject *>(QStringLiteral("fullImage")) : nullptr;
                // Image.Ready; a photo without a medium has its full size alone.
                if (!sharp || !full || full->property("status").toInt() != 1 || s->property("sharp").toString().isEmpty())
                    continue;
                const qreal opacity = full->property("opacity").toReal();
                if (opacity > 0 && opacity < 1) {
                    ++fades->midway;
                    if (sharp->property("status").toInt() != 1)
                        ++fades->bare;
                } else if (opacity >= 1 && sharp->property("source").toUrl().isEmpty()) {
                    ++fades->dropped;
                }
            }
        });
        watch->start();

        // The steps, each starting the next.
        auto still = std::make_shared<std::function<void()>>();
        auto fadeHeld = std::make_shared<std::function<void()>>();
        auto stopped = std::make_shared<std::function<void(QObject *, QObject *)>>();
        auto bare = std::make_shared<std::function<void()>>();
        auto fromViewer = std::make_shared<std::function<void()>>();

        // Its full size in, where the screen has the pixels for it (a while
        // at most). Come while the slide shows (here it came beneath,
        // unseen, at once): it fades in over the medium, which is let go
        // only then. Then held still, as on a workspace not shown: Qt finds
        // the window unexposed there only once a frame is asked of it, and
        // the screen is let go within seconds.
        *still = [=, &library] {
            // A photo with both, the frame on it alone: a wide one, which
            // covers a wide window, its medium soft at that size. Its full
            // size here, so that nothing waits on the network.
            const QList<int> found = findHere(library, QStringLiteral("full"), QStringLiteral(
                "NOT a.is_video AND EXISTS (SELECT 1 FROM renditions r WHERE r.asset_id = a.id AND r.kind = 'medium' "
                "AND r.width > r.height)"), 1);
            const int index = found.isEmpty() ? -1 : found[0];
            if (index >= 0) {
                const QString id = QString::fromUtf8(library.at(index).id);
                QMetaObject::invokeMethod(frame, "play", Q_ARG(QVariant, id), Q_ARG(QVariant, QVariantList{id}),
                                          Q_ARG(QVariant, false), Q_ARG(QVariant, QVariant()));
            }
            QObject *first = slide("current");
            QObject *sharp = first->findChild<QObject *>(QStringLiteral("sharpImage"));
            auto since = std::make_shared<QElapsedTimer>();
            since->start();
            auto held = [=] {
                QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, true));
                QTimer::singleShot(5000, window, [=] {
                    if (wayland)
                        check(!awake->inhibiting() && !awake->shows(),
                              QStringLiteral("held still on a workspace not shown, the screen is let go "
                                             "(shows %1, inhibiting %2)")
                                  .arg(awake->shows())
                                  .arg(awake->inhibiting()));
                    QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, false));
                    // All the photos again, for the rest.
                    QMetaObject::invokeMethod(frame->property("plugin").value<QObject *>(), "begin",
                                              Q_ARG(QVariant, QStringLiteral("all")));
                    (*waitFor)([=] { return slide("current")->property("ready").toBool(); }, 15000,
                               "the first photo comes again", *fadeHeld);
                });
            };
            (*waitFor)([=] { return (first->property("ready").toBool() && first->property("sharpest").toString() == "full")
                                    || since->elapsed() > 20000; },
                       25000, "the full size comes", [=] {
                const QString file = first->property("full").toString();
                if (first->property("sharpest").toString() != "full" || first->property("sharp").toString().isEmpty()) {
                    std::printf("selftest: no full size over a medium to fade in (%s: as sharp as its %s, after %lld ms)\n",
                                qPrintable(first->property("photoId").toString()),
                                qPrintable(first->property("sharpest").toString()), since->elapsed());
                    return held();
                }
                first->setProperty("full", QString());
                (*waitFor)([=] { return sharp->property("status").toInt() == 1; }, 5000, "the medium comes back", [=] {
                    *fades = {};
                    first->setProperty("full", file);
                    // Until the watch has seen it let go, not as it is:
                    // asked on its own, every 20 ms, it could be seen
                    // before the watch's next look.
                    (*waitFor)([=] { return fades->dropped > 0; },
                               10000, "the full size fades in, and the medium is let go", [=] {
                        check(fades->midway > 0 && fades->bare == 0,
                              QStringLiteral("the full size fades in over the medium, not the thumbnail "
                                             "(%1 of %2 looks without it)")
                                  .arg(fades->bare)
                                  .arg(fades->midway));
                        check(true, "then the medium is let go");
                        held();
                    });
                });
            });
        };

        // On to the next, and held still halfway through the fade: both
        // slides, and the fade, stay where they are.
        *fadeHeld = [=] {
            QObject *outgoing = slide("current");
            QMetaObject::invokeMethod(frame, "go", Q_ARG(QVariant, 1), Q_ARG(QVariant, false));
            (*waitFor)([=] { return outgoing->property("opacity").toReal() < 0.6; }, 10000, "a fade begins", [=] {
                QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, true));
                QObject *incoming = slide("next");
                const qreal opacity = outgoing->property("opacity").toReal();
                const QVector3D in = incoming->property("framed").value<QVector3D>();
                const QVector3D out = outgoing->property("framed").value<QVector3D>();
                QTimer::singleShot(1500, window, [=] {
                    check(qFuzzyCompare(outgoing->property("opacity").toReal(), opacity),
                          QStringLiteral("the fade holds still (%1 → %2)").arg(opacity).arg(outgoing->property("opacity").toReal()));
                    check(incoming->property("framed").value<QVector3D>() == in, "the photo coming holds still");
                    check(outgoing->property("framed").value<QVector3D>() == out, "the photo going holds still");
                    QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, false));
                    (*waitFor)([=] { return slide("current") == incoming; }, 3000, "the fade goes on to its end", [=] {
                        check(true, "the fade goes on to its end");
                        check(incoming->property("framed").value<QVector3D>() != in, "the photo moves again");
                        // A step back waiting, held still, as the library is
                        // read again: still back, not on to the next.
                        const int at = frame->property("at").toInt();
                        QMetaObject::invokeMethod(frame, "go", Q_ARG(QVariant, -1), Q_ARG(QVariant, false));
                        QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, true));
                        QMetaObject::invokeMethod(frame, "libraryChanged");
                        check(frame->property("target").toInt() == at - 1,
                              QStringLiteral("a step back waiting, the library read again: still back (%1, from %2)")
                                  .arg(frame->property("target").toInt())
                                  .arg(at));
                        QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, false));
                        (*stopped)(incoming, outgoing);
                    });
                });
            });
        };

        // Stopped: the screen let go at once, the photo fading out with the
        // frame, and then let go too.
        *stopped = [=](QObject *incoming, QObject *outgoing) {
            QMetaObject::invokeMethod(frame, "stop");
            check(!awake->wanted() && !awake->inhibiting(), "stopped, the screen may sleep again");
            QTimer::singleShot(100, window, [=] {
                check(frame->property("opacity").toReal() > 0.05 && !incoming->property("photoId").toString().isEmpty(),
                      "the photo fades out, not black");
                QTimer::singleShot(600, window, [=] {
                    check(incoming->property("photoId").toString().isEmpty()
                              && outgoing->property("photoId").toString().isEmpty(),
                          "then the slides let their photos go");
                    // Stopped while waiting for a photo, held still: shown
                    // again, it starts afresh, not where that wait was going.
                    QObject *plugin = frame->property("plugin").value<QObject *>();
                    QMetaObject::invokeMethod(plugin, "begin", Q_ARG(QVariant, QStringLiteral("all")));
                    QMetaObject::invokeMethod(frame, "go", Q_ARG(QVariant, 3), Q_ARG(QVariant, false));
                    QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, true));
                    QMetaObject::invokeMethod(frame, "stop");
                    QMetaObject::invokeMethod(plugin, "begin", Q_ARG(QVariant, QStringLiteral("all")));
                    check(frame->property("target").toInt() == -1 && !frame->property("paused").toBool(),
                          "shown again, no wait is left over");
                    QMetaObject::invokeMethod(frame, "go", Q_ARG(QVariant, 1), Q_ARG(QVariant, false));
                    check(frame->property("target").toInt() == 1,
                          QStringLiteral("the next is the one after the first (%1)").arg(frame->property("target").toInt()));
                    QMetaObject::invokeMethod(frame, "stop");
                    QTimer::singleShot(500, window, *bare);
                });
            });
        };

        // A photo with no medium rendition (one in eight of a library)
        // shows sharp: its full size in the medium's place.
        *bare = [=, &library] {
            const QList<int> found = findHere(library, QStringLiteral("full"), QStringLiteral(
                "NOT a.is_video AND NOT EXISTS (SELECT 1 FROM renditions r WHERE r.asset_id = a.id AND r.kind = 'medium')"), 1);
            const int index = found.isEmpty() ? -1 : found[0];
            if (index < 0) {
                std::printf("selftest: no photo without a medium in the library\n");
                return (*fromViewer)();
            }
            const QString id = QString::fromUtf8(library.at(index).id);
            QMetaObject::invokeMethod(frame, "play", Q_ARG(QVariant, id), Q_ARG(QVariant, QVariantList{id}),
                                      Q_ARG(QVariant, false), Q_ARG(QVariant, QVariant()));
            QObject *shown = slide("current");
            (*waitFor)([=] { return shown->property("ready").toBool() && shown->property("sharpest").toString() == "full"; },
                       30000, "a photo without a medium shows sharp, as its full size", [=] {
                check(!shown->property("missing").toBool(),
                      QStringLiteral("a photo without a medium shows sharp, as its full size (%1)").arg(id));
                QMetaObject::invokeMethod(frame, "stop");
                QTimer::singleShot(500, window, *fromViewer);
            });
        };

        // Last: started from a photo full window, then the plugin turned
        // off while it shows. The viewer it was started from shrinks away,
        // not left full window over the grid, dead, with the keys the
        // grid's.
        *fromViewer = [=, &library] {
            watch->stop();
            QObject *viewer = qmlEngine(window)->rootContext()->contextProperty("viewer").value<QObject *>();
            auto photoViewer = [window] { return window->findChild<QObject *>(QStringLiteral("photoViewer")); };
            const QString id = QString::fromUtf8(library.at(0).id);
            QMetaObject::invokeMethod(viewer, "openAt", Q_ARG(int, 0), Q_ARG(QRectF, QRectF(0, 0, 100, 100)));
            (*waitFor)([=] { return photoViewer() && photoViewer()->property("progress").toReal() == 1; }, 5000,
                       "a photo opens full window", [=] {
                QMetaObject::invokeMethod(frame, "play", Q_ARG(QVariant, id), Q_ARG(QVariant, QVariantList{id}),
                                          Q_ARG(QVariant, true), Q_ARG(QVariant, QVariant()));
                check(frame->property("shown").toBool() && !viewer->property("open").toBool(),
                      "started from the viewer, the frame shows and the viewer closes");
                QTimer::singleShot(100, window, [=] {
                    // The frame goes with its plugin.
                    const QPointer<QQuickItem> removed(frame);
                    plugins->setEnabled(QStringLiteral("pinhole.frame"), false);
                    (*waitFor)([=] { return !removed && !photoViewer()->property("shown").toBool()
                                            && photoViewer()->property("progress").toReal() == 0; },
                               3000, "the plugin turned off, no photo is left full window over the grid", [=] {
                        check(true, "the plugin turned off, no photo is left full window over the grid");
                        QTimer::singleShot(300, window, finish);
                    });
                });
            });
        };

        // Started from the grid, on all the photos.
        QMetaObject::invokeMethod(frame->property("plugin").value<QObject *>(), "begin", Q_ARG(QVariant, QStringLiteral("all")));
        check(frame->property("shown").toBool(), "the frame shows");
        check(awake->wanted(), "it asks for the screen to stay on");
        // Not on a workspace Hyprland is not showing (where a self-test
        // runs), once the window's frames stop coming there.
        if (wayland)
            check(awake->inhibiting() == awake->shows(),
                  QStringLiteral("an idle inhibitor is on the window's surface while it shows (%1)")
                      .arg(awake->shows() ? "it shows" : "not shown, none"));
        (*waitFor)([=] { return slide("current")->property("ready").toBool(); }, 15000, "the first photo comes",
                   [=] { (*still)(); });
    });
}
