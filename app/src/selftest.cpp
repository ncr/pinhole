#include "selftest.h"

#include "incoming.h"
#include "library.h"
#include "outgoing.h"
#include "photogrid.h"
#include "photoprovider.h"
#include "viewer.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QProcess>
#include <QQmlContext>
#include <QRegularExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <qpa/qplatformdrag.h>
#include <qpa/qwindowsysteminterface.h>
#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <memory>

namespace {

// The library position of the newest asset matching an SQL condition.
int findIndex(const Library &library, const QString &where) {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "selftest");
    db.setDatabaseName(library.dir() + "/library.sqlite");
    db.setConnectOptions("QSQLITE_OPEN_READONLY");
    QByteArray id;
    if (db.open()) {
        QSqlQuery q(db);
        q.exec("SELECT a.id FROM assets a WHERE " + where + " ORDER BY a.asset_date DESC LIMIT 1");
        if (q.next())
            id = q.value(0).toString().toUtf8();
    }
    db.close();
    QSqlDatabase::removeDatabase("selftest");
    for (int i = library.count() - 1; i >= 0; --i)
        if (library.at(i).id == id)
            return i;
    return -1;
}

struct Case {
    const char *name;
    QString where;
    bool live = false;
    // A Live Photo shown with this effect: the one set on the iPhone, or
    // made by the service when it is another.
    QString effect = {};
};

} // namespace

// Plays `source` as the viewer would and reports when the first frame is
// decoded, then the same after seeking to the middle.
static void playFirstFrame(const QUrl &source, const char *name, std::shared_ptr<QElapsedTimer> clock,
                           std::function<void()> done) {
    auto *player = new QMediaPlayer;
    auto *sink = new QVideoSink(player);
    player->setVideoSink(sink);
    auto seeked = std::make_shared<bool>(false);
    auto seekClock = std::make_shared<QElapsedTimer>();
    QObject::connect(sink, &QVideoSink::videoFrameChanged, player, [=](const QVideoFrame &frame) {
        if (!frame.isValid())
            return;
        if (!*seeked) {
            std::printf("selftest: %-11s first frame %dx%d after %5lld ms\n", name, frame.width(), frame.height(),
                        clock->elapsed());
            std::fflush(stdout);
            *seeked = true;
            seekClock->start();
            player->setPosition(player->duration() / 2);
            return;
        }
        if (frame.startTime() / 1000 < player->duration() / 2 - 1000)
            return; // still a frame from before the seek
        std::printf("selftest: %-11s frame at %llds after seeking, %lld ms\n", name,
                    (long long)(frame.startTime() / 1000000), seekClock->elapsed());
        std::fflush(stdout);
        player->stop();
        player->deleteLater();
        done();
    });
    QObject::connect(player, &QMediaPlayer::errorOccurred, player, [=](QMediaPlayer::Error, const QString &message) {
        std::printf("selftest: %-11s does not play: %s\n", name, qPrintable(message));
        std::fflush(stdout);
        player->deleteLater();
        done();
    });
    player->setSource(source);
    player->play();
}

// Opens each kind of asset in the viewer, as a person would, and reports
// how long each stage took to arrive and whether the files decode.
void runViewerSelfTest(const Library &library, Viewer &viewer) {
    const QList<Case> cases = {
        {"HEIC photo", "EXISTS (SELECT 1 FROM renditions r WHERE r.asset_id = a.id AND r.kind = 'original' AND r.file_type = 'public.heic') AND NOT a.is_live"},
        {"JPEG photo", "EXISTS (SELECT 1 FROM renditions r WHERE r.asset_id = a.id AND r.kind = 'original' AND r.file_type = 'public.jpeg')"},
        {"video", "a.is_video AND a.duration < 60"},
        {"long video", "a.is_video AND a.duration > 300"},
        {"Live Photo", "a.is_live", true},
        {"loop", "a.live_effect = 'loop'", true, "loop"},
        {"bounce", "a.live_effect = 'bounce'", true, "bounce"},
        {"long exp.", "a.live_effect = 'long_exposure'", true, "long_exposure"},
        {"made bounce", "a.is_live AND a.live_effect IS NULL", true, "bounce"},
        {"made long", "a.is_live AND a.live_effect IS NULL", true, "long_exposure"},
    };
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [&library, &viewer, cases, next](int k) {
        if (k >= cases.size()) {
            std::printf("selftest: done\n");
            std::fflush(stdout);
            QCoreApplication::quit();
            return;
        }
        const Case c = cases[k];
        const int index = findIndex(library, c.where);
        if (index < 0) {
            std::printf("selftest: %s: none in the library\n", c.name);
            (*next)(k + 1);
            return;
        }
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto connections = std::make_shared<QList<QMetaObject::Connection>>();
        auto finish = [connections, next, k] {
            for (const auto &c : *connections)
                QObject::disconnect(c);
            QTimer::singleShot(0, [next, k] { (*next)(k + 1); });
        };
        auto report = [clock](const char *name, const char *what, const QString &file) {
            QElapsedTimer decode;
            decode.start();
            const QImage image = file.isEmpty() ? QImage() : decodePhoto(file, QSize());
            std::printf("selftest: %-11s %-9s after %5lld ms%s\n", name, what, clock->elapsed(),
                        file.isEmpty() ? "" : qPrintable(QStringLiteral("; decodes to %1x%2 in %3 ms")
                                                             .arg(image.width()).arg(image.height()).arg(decode.elapsed())));
            std::fflush(stdout);
        };
        auto pathOf = [](const QUrl &url) {
            return QUrl::fromPercentEncoding(url.toString().mid(QStringLiteral("image://photo/").size()).toUtf8());
        };
        connections->append(QObject::connect(&viewer, &Viewer::imageChanged, &viewer, [&viewer, c, report, pathOf, finish] {
            if (viewer.image().isEmpty())
                return;
            report(c.name, qPrintable(viewer.stage()), pathOf(viewer.image()));
            if (viewer.stage() == QLatin1String("original") && c.effect.isEmpty()) {
                if (c.live)
                    viewer.playLive();
                else
                    finish();
            }
        }));
        connections->append(QObject::connect(&viewer, &Viewer::effectChanged, &viewer, [&viewer, c, report, pathOf, finish] {
            if (c.effect != QLatin1String("long_exposure") || viewer.longExposure().isEmpty())
                return;
            report(c.name, "effect", pathOf(viewer.longExposure()));
            finish();
        }));
        connections->append(QObject::connect(&viewer, &Viewer::videoChanged, &viewer, [&viewer, c, report, clock, finish] {
            if (viewer.videoSource().isEmpty())
                return;
            report(c.name, viewer.videoSource().isLocalFile() ? "file" : "stream", QString());
            playFirstFrame(viewer.videoSource(), c.name, clock, finish);
        }));
        connections->append(QObject::connect(&viewer, &Viewer::liveChanged, &viewer, [&viewer, c, report, clock, finish] {
            if (viewer.liveSource().isEmpty())
                return;
            report(c.name, c.effect.isEmpty() ? "motion" : "effect", QString());
            if (c.effect.isEmpty())
                finish();
            else
                playFirstFrame(viewer.liveSource(), c.name, clock, finish);
        }));
        connections->append(QObject::connect(&viewer, &Viewer::messageChanged, &viewer, [&viewer, c] {
            if (!viewer.message().isEmpty())
                std::printf("selftest: %-11s says \"%s\"\n", c.name, qPrintable(viewer.message()));
        }));
        // Nothing within a minute is a failure too.
        QTimer::singleShot(60000, &viewer, [c, finish, clock] {
            if (clock->elapsed() >= 59000) {
                std::printf("selftest: %s: timed out\n", c.name);
                finish();
            }
        });
        viewer.openAt(index, QRectF(0, 0, 100, 100));
        if (!c.effect.isEmpty()) {
            std::printf("selftest: %-11s set on the iPhone: %s\n", c.name, qPrintable(viewer.savedEffect()));
            viewer.setEffect(c.effect);
        }
    };
    QTimer::singleShot(500, [next] { (*next)(0); });
}

void runOutgoingSelfTest(const Library &library, Outgoing &outgoing, const QString &folder) {
    const QList<int> picks = {
        findIndex(library, "EXISTS (SELECT 1 FROM renditions r WHERE r.asset_id = a.id AND r.kind = 'original' AND r.file_type = 'public.heic') AND NOT a.is_live"),
        findIndex(library, "EXISTS (SELECT 1 FROM renditions r WHERE r.asset_id = a.id AND r.kind = 'original' AND r.file_type = 'public.jpeg')"),
        findIndex(library, "a.is_video AND a.duration < 30"),
    };
    auto describe = [](const QStringList &files) {
        for (const QString &f : files) {
            QProcess identify;
            identify.start("magick", {"identify", "-format", "%m %wx%h, taken %[EXIF:DateTimeOriginal]", f + "[0]"});
            identify.waitForFinished(20000);
            const QString what = QString::fromUtf8(identify.readAllStandardOutput()).trimmed();
            std::printf("selftest:   %-28s %8lld KB  %s\n", qPrintable(QFileInfo(f).fileName()), QFileInfo(f).size() / 1024,
                        qPrintable(what.isEmpty() ? QStringLiteral("(video)") : what));
        }
    };
    QTimer::singleShot(500, [&outgoing, picks, folder, describe, &library] {
        std::printf("selftest: originals\n");
        outgoing.prepare(outgoing.snapshot(picks), false, folder + "/originals", [&outgoing, picks, folder, describe, &library](const QStringList &files, int failed) {
            describe(files);
            std::printf("selftest:   %d failed\nselftest: compatible\n", failed);
            outgoing.prepare(outgoing.snapshot(picks), true, folder + "/compatible", [&outgoing, picks, describe, &library](const QStringList &files, int failed) {
                describe(files);
                std::printf("selftest:   %d failed\nselftest: copy\n", failed);
                outgoing.copy({picks.first()});
                // Copying sets the clipboard once the file is ready.
                QTimer::singleShot(8000, [&library, picks] {
                    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
                    std::printf("selftest:   clipboard: %lld file(s), image/png %s (%lld KB), image/jpeg %s, gnome-copied-files %s\n",
                                mime ? qlonglong(mime->urls().size()) : 0LL,
                                mime && mime->hasFormat("image/png") ? "yes" : "no",
                                mime ? qlonglong(mime->data("image/png").size() / 1024) : 0LL,
                                mime && mime->hasFormat("image/jpeg") ? "yes" : "no",
                                mime && mime->hasFormat("x-special/gnome-copied-files") ? "yes" : "no");
                    if (mime && !mime->urls().isEmpty())
                        std::printf("selftest:   first: %s (camera name %s)\n", qPrintable(mime->urls().first().fileName()),
                                    qPrintable(library.at(picks.first()).filename));
                    std::printf("selftest: done\n");
                    std::fflush(stdout);
                    QCoreApplication::quit();
                });
            });
        });
    });
}

void runReloadSelfTest(const Library &library, PhotoGrid *grid, Viewer &viewer) {
    QTimer::singleShot(300, grid, [&library, grid, &viewer] {
        const int before = library.count();
        const QByteArray open = library.at(before - 10).id;
        const QByteArray b = library.at(before - 1).id;
        viewer.openAt(before - 10, QRectF(0, 0, 10, 10));
        // Everything selected: after the reload every one of these should
        // still be, and none of the new photos.
        grid->selectAll();
        std::printf("selftest: %d items; viewer on %s; waiting for the service\n", before, open.constData());
        std::fflush(stdout);
        QObject::connect(grid, &PhotoGrid::libraryChanged, grid, [&library, grid, &viewer, before, open, b] {
            QTimer::singleShot(500, grid, [&library, grid, &viewer, before, open, b] {
                const bool viewerKept = viewer.isOpen() && library.at(viewer.index()).id == open;
                std::printf("selftest: reloaded: %d items (was %d)\n", library.count(), before);
                std::printf("selftest: viewer still on the same photo: %s\n", viewerKept ? "yes" : "no");
                std::printf("selftest: grid at the newest end: %s\n", grid->scrollFraction() > 0.999 ? "yes" : "no");
                std::printf("selftest: selection followed its photos: %d selected (expected %d)\n", grid->selectionCount(), before);
                std::printf("selftest: newest is now %s (was %s)\n", library.at(library.count() - 1).filename.toUtf8().constData(),
                            qPrintable(QString::fromUtf8(b)));
                std::printf("selftest: done\n");
                std::fflush(stdout);
                QCoreApplication::quit();
            });
        });
        QTimer::singleShot(60000, [] {
            std::printf("selftest: no change announced within a minute\n");
            QCoreApplication::quit();
        });
    });
}

namespace {

// Synthetic events carry this clock, on the touchpad's: a press is judged
// against when a coast began.
quint64 eventClock = 100000;

void mouse(QQuickWindow *window, QEvent::Type type, QPointF at, Qt::MouseButton button,
           Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, at, window->mapToGlobal(at), button, held, mods);
    event.setTimestamp(eventClock += 8);
    QGuiApplication::sendEvent(window, &event);
}

void click(QQuickWindow *window, QPointF at, Qt::MouseButton button = Qt::LeftButton,
           Qt::KeyboardModifiers mods = Qt::NoModifier) {
    mouse(window, QEvent::MouseButtonPress, at, button, mods);
    mouse(window, QEvent::MouseButtonRelease, at, button, mods);
}

// A key pressed and let go, as the window system brings it: shortcuts
// first, as typed.
void key(QQuickWindow *window, int code, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString &text = {}) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, mods, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, mods, text);
}

// The centre of a named item, in the window.
QPointF centreOf(QQuickWindow *window, const char *name) {
    auto *item = window->findChild<QQuickItem *>(QString::fromLatin1(name));
    return item ? item->mapToScene(QPointF(item->width() / 2, item->height() / 2)) : QPointF(-1, -1);
}

void doubleClick(QQuickWindow *window, QPointF at) {
    click(window, at);
    mouse(window, QEvent::MouseButtonPress, at, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonDblClick, at, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, at, Qt::LeftButton);
}

void wheel(QQuickWindow *window, QPointF at, QPoint pixels, QPoint angle,
           Qt::ScrollPhase phase = Qt::NoScrollPhase, quint64 time = 0) {
    QWheelEvent event(at, window->mapToGlobal(at), pixels, angle, Qt::NoButton, Qt::NoModifier, phase, false);
    event.setTimestamp(time);
    QGuiApplication::sendEvent(window, &event);
}

// A pinch to `scale` (from 1) at `at`, as the touchpad reports it: begin,
// changes in scale, end.
void pinch(QQuickWindow *window, QPointF at, double scale) {
    auto send = [&](Qt::NativeGestureType type, double value) {
        QNativeGestureEvent event(type, QPointingDevice::primaryPointingDevice(), 2, at, at, window->mapToGlobal(at),
                                  value, QPointF());
        QGuiApplication::sendEvent(window, &event);
    };
    send(Qt::BeginNativeGesture, 0);
    for (int i = 0; i < 10; ++i)
        send(Qt::ZoomNativeGesture, (scale - 1) / 10);
    send(Qt::EndNativeGesture, 0);
}

// Two fingers moving `steps` times by `by` px, 8 ms apart, as a touchpad
// on Wayland reports it: a begin with no movement, updates, an end.
void swipe(QQuickWindow *window, QPointF at, QPoint by, int steps) {
    eventClock += 1000;
    wheel(window, at, {}, {}, Qt::ScrollBegin, eventClock);
    for (int i = 0; i < steps; ++i)
        wheel(window, at, by, by * 8, Qt::ScrollUpdate, eventClock += 8);
    wheel(window, at, {}, {}, Qt::ScrollEnd, eventClock += 8);
}

} // namespace

// The left button held from `at` and the pointer moved `by` in ten steps,
// then let go there.
void drag(QQuickWindow *window, QPointF at, QPointF by) {
    mouse(window, QEvent::MouseButtonPress, at, Qt::LeftButton);
    for (int i = 1; i <= 10; ++i)
        mouse(window, QEvent::MouseMove, at + by * (i / 10.0), Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, at + by, Qt::LeftButton);
}

void runMouseSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library) {
    struct Step {
        const char *what;
        std::function<void()> act;
        std::function<bool()> check;
        int wait = 600; // long enough for the viewer's animations and the swipe's pause
    };
    auto video = [window] { return window->findChild<QObject *>(QStringLiteral("photoViewer")); };
    auto at = [video] { return video()->property("videoPosition").toDouble(); };
    auto length = [video] { return video()->property("videoDuration").toDouble(); };
    const int clip = findIndex(library, "a.is_video AND a.duration BETWEEN 40 AND 300");
    auto mark = std::make_shared<double>(0);
    auto start = std::make_shared<int>(-1);
    // The grid's size, which is the content's: tiled on screen, the window
    // (and its root item) can still report the size it asked for while the
    // compositor gave it another.
    // Worked out at each use: the compositor may size the window after the
    // test has begun.
    auto middle = [grid] { return QPointF(grid->width() / 2.0, grid->height() / 2.0); };
    auto atRight = [grid] { return QPointF(grid->width() - 40.0, grid->height() / 2.0); };
    auto atLeft = [grid] { return QPointF(40.0, grid->height() / 2.0); };
    auto w = [grid] { return grid->width(); };
    auto h = [grid] { return grid->height(); };
    // No menu anywhere: a two-finger tap (the right button) is only a touch.
    auto popups = [window] { return window->findChildren<QObject *>(QRegularExpression("Menu|Popup")).size(); };
    // Copy's tick seen, and anything in the status line meanwhile.
    auto ticked = std::make_shared<bool>(false);
    auto bottomLine = std::make_shared<bool>(false);
    auto columns = std::make_shared<int>(0);
    auto steps = std::make_shared<QList<Step>>(QList<Step>{
        {"three notches of a horizontal wheel zoom in a level, as a pinch",
         [=] {
             *columns = grid->columns();
             for (int i = 0; i < 3; ++i)
                 wheel(window, middle(), {15, 0}, {120, 0});
         },
         [=] { return grid->columns() < *columns && !grid->zooming(); }, 1200},
        {"three back zoom out to where it was",
         [=] {
             for (int i = 0; i < 3; ++i)
                 wheel(window, middle(), {-15, 0}, {-120, 0});
         },
         [=] { return grid->columns() == *columns && !grid->zooming(); }, 1200},
        {"a flick sets the grid coasting", [=] { swipe(window, middle(), {0, 30}, 8); },
         [=] { return grid->animating(); }, 100},
        {"a two-finger tap stops it, and opens nothing", [=] { click(window, middle(), Qt::RightButton); },
         [=, &viewer] { return !grid->animating() && !viewer.isOpen() && grid->selectionCount() == 0 && popups() == 0; }},
        {"a two-finger tap on a photo opens nothing",
         [=] {
             *start = grid->indexAt(middle().x(), middle().y());
             click(window, middle(), Qt::RightButton);
         },
         [=, &viewer] { return !viewer.isOpen() && grid->selectionCount() == 0 && popups() == 0; }},
        {"a click opens the photo, selecting nothing", [=] { click(window, middle()); },
         [=, &viewer] { return viewer.isOpen() && viewer.index() == *start && grid->selectionCount() == 0; }, 900},
        {"clicking the right side shows the next", [=] { click(window, atRight()); },
         [&viewer, start] { return viewer.index() == *start + 1; }},
        {"clicking the left side shows the previous", [=] { click(window, atLeft()); },
         [&viewer, start] { return viewer.index() == *start; }},
        // As an MX Master sends a notch on Wayland: with 15 px. Natural
        // (Apple) direction, the system's being classic.
        {"a thumb-wheel notch shows the next", [=] { wheel(window, middle(), {15, 0}, {120, 0}); },
         [&viewer, start] { return viewer.index() == *start + 1; }},
        {"two notches of the wheel pulled towards you zoom in",
         [=] {
             wheel(window, middle(), {0, -15}, {0, -120});
             wheel(window, middle(), {0, -15}, {0, -120});
         },
         [&viewer, start] {
             // A video does not zoom; which one this is depends on the library.
             if (viewer.property("video").toBool()) {
                 std::printf("selftest:      (a video, which does not zoom)\n");
                 return viewer.index() == *start + 1;
             }
             return viewer.index() == *start + 1 && std::abs(viewer.property("zoom").toDouble() - 1.5625) < 0.02;
         }, 800},
        {"and two pushed away fit it again",
         [=] {
             wheel(window, middle(), {0, 15}, {0, 120});
             wheel(window, middle(), {0, 15}, {0, 120});
         },
         [&viewer] { return std::abs(viewer.property("zoom").toDouble() - 1) < 0.01; }, 800},
        {"a long swipe to the left shows the next, once", [=] { swipe(window, middle(), {-12, 2}, 30); },
         [&viewer, start] { return viewer.index() == *start + 2; }},
        {"two short swipes to the right show two before", [=] {
             swipe(window, middle(), {8, -1}, 8);
             swipe(window, middle(), {8, 1}, 8);
         },
         [&viewer, start] { return viewer.index() == *start; }},
        {"a swipe left again shows the next", [=] { swipe(window, middle(), {-10, 0}, 10); },
         [&viewer, start] { return viewer.index() == *start + 1; }},
        {"a small drag up springs back, the photo staying open", [=] { swipe(window, middle(), {0, -2}, 6); },
         [&viewer, start] { return viewer.isOpen() && viewer.index() == *start + 1 && viewer.property("dismiss").toDouble() == 0; }},
        {"a slightly slanted swipe still counts", [=] { swipe(window, middle(), {-10, 5}, 10); },
         [&viewer, start] { return viewer.index() == *start + 2; }},
        {"mid-swipe the photo follows the fingers",
         [=] {
             wheel(window, middle(), {}, {}, Qt::ScrollBegin, 900000);
             for (int i = 0; i < 5; ++i)
                 wheel(window, middle(), {-10, 0}, {-80, 0}, Qt::ScrollUpdate, 900000 + 8 * (i + 1));
         },
         [&viewer] { return viewer.property("offset").toDouble() < -80; }, 100},
        {"stopping before lifting springs back to the same photo",
         [=] { wheel(window, middle(), {}, {}, Qt::ScrollEnd, 900400); },
         [&viewer, start] { return viewer.index() == *start + 2 && std::abs(viewer.property("offset").toDouble()) < 1; }, 900},
        {"a pinch zooms in", [=] { pinch(window, middle(), 2.0); },
         [&viewer] { return std::abs(viewer.property("zoom").toDouble() - 2.0) < 0.05; }},
        {"zoomed, a swipe pans instead of paging", [=] { swipe(window, middle(), {-6, -6}, 20); },
         [&viewer, start] {
             // Across or down, as far as the zoomed photo has room to move.
             const double moved = std::abs(viewer.property("panX").toDouble()) + std::abs(viewer.property("panY").toDouble());
             return viewer.index() == *start + 2 && std::abs(viewer.property("offset").toDouble()) < 1 && moved > 50;
         }},
        {"zoomed to 3x in the middle, room to coast either way",
         [=, &viewer] {
             QMetaObject::invokeMethod(&viewer, "resetZoom");
             QMetaObject::invokeMethod(&viewer, "zoomBy", Q_ARG(qreal, 3.0), Q_ARG(qreal, w() / 2), Q_ARG(qreal, (h() - 60) / 2)); // the photo area, above the carousel
         },
         [&viewer] {
             const double zoom = viewer.property("zoom").toDouble(), y = viewer.property("panY").toDouble();
             if (std::abs(zoom - 3) >= 0.05 || std::abs(y) >= 2)
                 std::printf("selftest:      (zoom %.3f, pan %.1f,%.1f)\n", zoom, viewer.property("panX").toDouble(), y);
             return std::abs(zoom - 3) < 0.05 && std::abs(y) < 2;
         }, 800},
        {"a flick on a zoomed photo coasts on after the fingers lift",
         [=, &viewer] {
             swipe(window, middle(), {0, 3}, 6); // gentle: it stays well inside the photo's edges
             *mark = viewer.property("panY").toDouble();
         },
         [=, &viewer] { return viewer.property("panY").toDouble() > *mark + 30; }, 250},
        {"a flick back sets it coasting again", [=] { swipe(window, middle(), {0, -3}, 6); },
         [=, &viewer] {
             *mark = viewer.property("panY").toDouble();
             return true;
         }, 30},
        {"a two-finger tap stops the coast", [=] { click(window, middle(), Qt::RightButton); },
         [=, &viewer] { return std::abs(viewer.property("panY").toDouble() - *mark) < 25 && viewer.isOpen(); }, 300},
        {"a double-click zoomed goes back to fitted", [=] { doubleClick(window, middle()); },
         [&viewer] { return viewer.isOpen() && viewer.property("zoom").toDouble() < 1.02; }, 900},
        {"clicking a thumbnail in the carousel slides there, the strip too",
         [=] { click(window, QPointF(w() / 2.0 + 3 * 38, h() - 30.0)); },
         [=, &viewer] {
             // Only the two photos slide: the one it came from sits beside the
             // new one, the strip travels the rest of the way.
             int left = -2;
             QMetaObject::invokeMethod(&viewer, "beside", Q_RETURN_ARG(int, left), Q_ARG(int, -1));
             return viewer.property("offset").toDouble() > 0 &&
                    viewer.property("stripOffset").toDouble() > viewer.property("offset").toDouble() &&
                    left == *start + 2;
         }, 40},
        {"clicking a thumbnail in the carousel shows that photo",
         [] {},
         [=, &viewer] {
             if (viewer.index() != *start + 5)
                 std::printf("selftest:      (shows %d, wanted %d)\n", viewer.index() - *start, 5);
             return viewer.index() == *start + 5;
         }},
        {"pinching closed goes back to the library", [=] { pinch(window, middle(), 0.5); },
         [&viewer] { return !viewer.isOpen(); }, 900},
        {"a click opens it again", [=] { click(window, grid->rectOf(*start + 5).center()); },
         [&viewer] { return viewer.isOpen(); }, 900},
        {"Copy above the photo answers with a tick on itself",
         [=] {
             // Watch the button's phase as it goes; the copy fetches the
             // original first.
             *ticked = false;
             auto *watch = new QTimer(window);
             QObject::connect(watch, &QTimer::timeout, window, [=] {
                 if (QObject *b = window->findChild<QObject *>(QStringLiteral("viewerCopy"))) {
                     const bool done = b->property("phase").toString() == QLatin1String("done");
                     // The tick as it shows, to look at (PINHOLE_SELFTEST_DIR).
                     if (done && !*ticked && !qEnvironmentVariable("PINHOLE_SELFTEST_DIR").isEmpty())
                         QTimer::singleShot(200, window, [window] {
                             window->grabWindow().save(qEnvironmentVariable("PINHOLE_SELFTEST_DIR") + "/copy-tick.png");
                         });
                     *ticked = *ticked || done;
                 }
                 *bottomLine = *bottomLine || !qmlContext(window->contentItem())
                                                      ->contextProperty("outgoing").value<QObject *>()
                                                      ->property("status").toString().isEmpty();
             });
             watch->start(30);
             QTimer::singleShot(6000, watch, &QObject::deleteLater);
             click(window, centreOf(window, "viewerCopy"));
         },
         [=] {
             const QMimeData *copied = QGuiApplication::clipboard()->mimeData();
             return *ticked && !*bottomLine && copied && copied->hasUrls();
         }, 6000},
        {"Copy again ticks again",
         [=] {
             auto *clock = new QElapsedTimer;
             clock->start();
             auto *watch = new QTimer(window);
             auto last = std::make_shared<QString>();
             QObject::connect(watch, &QTimer::timeout, window, [=] {
                 QObject *b = window->findChild<QObject *>(QStringLiteral("viewerCopy"));
                 const QString phase = b ? b->property("phase").toString() : QStringLiteral("?");
                 if (phase != *last) {
                     std::printf("selftest:      %5lld ms: %s\n", clock->elapsed(), qPrintable(phase));
                     *last = phase;
                 }
             });
             watch->start(10);
             QTimer::singleShot(7000, watch, [watch, clock] { watch->deleteLater(); delete clock; });
             click(window, centreOf(window, "viewerCopy"));
         },
         [] { return true; }, 7000},
        {"a two-finger tap in the viewer neither closes nor steps", [=, &viewer] {
             *mark = viewer.index();
             click(window, middle(), Qt::RightButton);
         },
         [=, &viewer] { return viewer.isOpen() && viewer.index() == int(*mark) && popups() == 0; }},
        {"a swipe down goes back to the library", [=] { swipe(window, middle(), {0, 12}, 12); },
         [&viewer] { return !viewer.isOpen(); }, 900},
        {"and a click opens a photo again", [=] { click(window, middle()); },
         [&viewer] { return viewer.isOpen(); }, 900},
        {"a short mouse drag down springs back and stays open", [=] { drag(window, middle(), {0, 30}); },
         [&viewer] { return viewer.isOpen() && viewer.dismiss() == 0; }},
        {"a mouse drag to the left shows the next", [=, &viewer] {
             *mark = viewer.index();
             drag(window, middle(), {-0.45 * w(), 0});
         },
         [=, &viewer] { return viewer.isOpen() && viewer.index() == int(*mark) + 1; }},
        {"a mouse drag down goes back to the library", [=] { drag(window, middle(), {0, 0.3 * h()}); },
         [&viewer] { return !viewer.isOpen(); }, 900},
        {"and a click opens a photo again", [=] { click(window, middle()); },
         [&viewer] { return viewer.isOpen(); }, 900},
        {"the mouse's back button goes back to the library", [=] { click(window, middle(), Qt::BackButton); },
         [&viewer] { return !viewer.isOpen(); }},
        {"a double-click opens it and leaves it open", [=] { doubleClick(window, grid->rectOf(*start).center()); },
         [&viewer] { return viewer.isOpen(); }, 900},
        {"double-click in the viewer goes back", [=] { doubleClick(window, middle()); },
         [&viewer] { return !viewer.isOpen(); }},
        {"the Select button enters select mode", [=] { click(window, centreOf(window, "selectButton")); },
         [=, &viewer] { return grid->selecting() && !viewer.isOpen(); }},
        {"in select mode a click ticks a photo instead of opening it",
         [=] { click(window, grid->rectOf(*start).center()); },
         [=, &viewer] { return !viewer.isOpen() && grid->selection() == QList<int>{*start}; }},
        {"Shift+click ticks the range", [=] { click(window, grid->rectOf(*start - 3).center(), Qt::LeftButton, Qt::ShiftModifier); },
         [=] { return grid->selectionCount() == 4; }},
        {"clicking a ticked photo unticks it", [=] { click(window, grid->rectOf(*start - 1).center()); },
         [=] { return grid->selectionCount() == 3; }},
        {"Done leaves select mode and clears it", [=] { click(window, centreOf(window, "selectButton")); },
         [=] { return !grid->selecting() && grid->selectionCount() == 0; }},
        {"Ctrl+click starts selecting with that photo",
         [=] { click(window, grid->rectOf(*start).center(), Qt::LeftButton, Qt::ControlModifier); },
         [=, &viewer] { return grid->selecting() && grid->selectionCount() == 1 && !viewer.isOpen(); }},
        {"and Done again", [=] { click(window, centreOf(window, "selectButton")); },
         [=] { return !grid->selecting(); }},
        {"a video opens and plays", [=, &viewer] { viewer.openAt(clip, grid->rectOf(clip)); },
         [=] { return clip >= 0 && length() > 0 && at() > 0; }, 6000},
        {"clicking three quarters along the bottom seeks there",
         // The bar runs along the top of the carousel.
         [=] { click(window, QPointF(w() * 0.75, h() - 60.0 - 8.0)); },
         [=] {
             *mark = at();
             return std::abs(*mark / length() - 0.75) < 0.03;
         }, 1500},
        {"Shift+Left goes back ten seconds",
         [=] { QMetaObject::invokeMethod(video(), "skip", Q_ARG(QVariant, -10)); },
         [=] { return at() < *mark - 5000 && at() > *mark - 12000; }, 1500},
        {"the video closes", [=] { click(window, middle(), Qt::BackButton); },
         [&viewer] { return !viewer.isOpen(); }},
        {"far from the newest photos, the Newest button shows", [=] { grid->jumpToFraction(0.3); },
         [=] {
             QObject *b = window->findChild<QObject *>(QStringLiteral("newestButton"));
             return !grid->atNewest() && b && b->property("opacity").toDouble() > 0.9;
         }},
        {"tapping it glides back to them, and it goes", [=] { click(window, centreOf(window, "newestButton")); },
         [=] {
             QObject *b = window->findChild<QObject *>(QStringLiteral("newestButton"));
             return grid->atNewest() && grid->scrollFraction() > 0.999 && b->property("opacity").toDouble() < 0.1;
         }, 1500},
        // The buttons' keys.
        {"away again, N glides back to the newest", [=] {
             grid->jumpToFraction(0.3);
             QTimer::singleShot(300, window, [=] { key(window, Qt::Key_N, {}, QStringLiteral("n")); });
         },
         [=] { return grid->atNewest() && grid->scrollFraction() > 0.999; }, 1800},
        {"Down scrolls a row", [=] {
             grid->jumpToFraction(0.5);
             *mark = grid->scrollFraction() * grid->scrollRange();
             QTimer::singleShot(100, window, [=] { key(window, Qt::Key_Down); });
         },
         [=] {
             const double moved = grid->scrollFraction() * grid->scrollRange() - *mark;
             return std::abs(moved - grid->rectOf(1).left() + grid->rectOf(0).left()) < 2;
         }, 1200},
        {"Shift+Up half a screen back", [=] {
             *mark = grid->scrollFraction() * grid->scrollRange();
             key(window, Qt::Key_Up, Qt::ShiftModifier);
         },
         [=] { return std::abs(*mark - grid->scrollFraction() * grid->scrollRange() - grid->height() * 0.5) < 2; }, 1200},
        {"Shift+Down held gathers speed", [=] {
             grid->jumpToFraction(0.1);
             auto *times = new QList<double>;
             auto *held = new QElapsedTimer;
             held->start();
             auto *repeat = new QTimer(window);
             QObject::connect(repeat, &QTimer::timeout, window, [=] {
                 const double t = held->nsecsElapsed() / 1e9;
                 const bool first = times->isEmpty();
                 QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
                     window, QEvent::KeyPress, Qt::Key_Down, Qt::ShiftModifier, {}, !first);
                 times->append(t);
                 const double at = grid->scrollFraction() * grid->scrollRange();
                 // Where it was a second in, and at the end.
                 if (std::abs(t - 1.0) < 0.02 || (t > 1.0 && *mark < 0))
                     *mark = at;
                 if (t > 3.2) {
                     repeat->stop();
                     QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
                         window, QEvent::KeyRelease, Qt::Key_Down, Qt::ShiftModifier);
                     std::printf("selftest: held a second in at %.0f, at the end %.0f\n", *mark, at);
                 }
             });
             *mark = -1;
             repeat->start(40);
         },
         [=] {
             // Speeding up: the last 2.2 s went much further than 2.2 s at
             // the starting speed (2,500 px/s).
             const double at = grid->scrollFraction() * grid->scrollRange();
             return *mark > 0 && at - *mark > 2.2 * 2500 * 1.6;
         }, 3800},
        {"S starts selecting", [=] { key(window, Qt::Key_S, {}, QStringLiteral("s")); },
         [=] { return grid->selecting(); }},
        {"a drag across the photos ticks every one from start to end", [=] {
             const int from = grid->indexAt(grid->width() * 0.2, grid->height() * 0.3);
             const int to = grid->indexAt(grid->width() * 0.7, grid->height() * 0.6);
             *mark = to - from + 1;
             const QPointF a = grid->rectOf(from).center(), b = grid->rectOf(to).center();
             mouse(window, QEvent::MouseButtonPress, a, Qt::LeftButton);
             for (int i = 1; i <= 10; ++i)
                 mouse(window, QEvent::MouseMove, a + (b - a) * (i / 10.0), Qt::LeftButton);
             mouse(window, QEvent::MouseButtonRelease, b, Qt::LeftButton);
         },
         [=] { return grid->selecting() && grid->selectionCount() == int(*mark); }},
        {"held at the top edge, the grid scrolls on and the ticks follow", [=] {
             grid->clearSelection();
             const QPointF a = grid->rectOf(grid->indexAt(grid->width() * 0.5, grid->height() * 0.7)).center();
             mouse(window, QEvent::MouseButtonPress, a, Qt::LeftButton);
             mouse(window, QEvent::MouseMove, a - QPointF(0, 20), Qt::LeftButton);
             mouse(window, QEvent::MouseMove, QPointF(a.x(), 4), Qt::LeftButton);
             QTimer::singleShot(700, window, [=] {
                 mouse(window, QEvent::MouseButtonRelease, QPointF(a.x(), 4), Qt::LeftButton);
             });
         },
         [=] {
             const int onScreen = grid->columns() * int(grid->height() / grid->rectOf(0).height());
             return grid->selectionCount() > onScreen && !grid->animating();
         }, 1200},
        {"S again is Done", [=] { key(window, Qt::Key_S, {}, QStringLiteral("s")); },
         [=] { return !grid->selecting() && grid->selectionCount() == 0; }},
        {"? shows the help", [=] { key(window, Qt::Key_Question, {}, QStringLiteral("?")); },
         [=] { return window->findChild<QObject *>(QStringLiteral("help"))->property("shown").toBool(); }},
        {"with the help up, the buttons' keys rest", [=] { key(window, Qt::Key_S, {}, QStringLiteral("s")); },
         [=] { return !grid->selecting(); }},
        {"Esc puts the help away", [=] { key(window, Qt::Key_Escape); },
         [=] { return !window->findChild<QObject *>(QStringLiteral("help"))->property("shown").toBool(); }},
        {"the ? button in the corner shows it", [=] { click(window, centreOf(window, "helpButton")); },
         [=] { return window->findChild<QObject *>(QStringLiteral("help"))->property("shown").toBool(); }},
        {"and Esc again puts it away", [=] { key(window, Qt::Key_Escape); },
         [=] { return !window->findChild<QObject *>(QStringLiteral("help"))->property("shown").toBool(); }},
        {"a photo open, Esc on its close button closes it", [=, &viewer] {
             const int index = grid->indexAt(grid->width() / 2, grid->height() / 2);
             viewer.openAt(index, grid->rectOf(index));
             QTimer::singleShot(600, window, [=] { key(window, Qt::Key_Escape); });
         },
         [&viewer] { return !viewer.isOpen(); }, 1200},
    });
    Viewer *viewerPtr = &viewer;
    auto failures = std::make_shared<int>(0);
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=](int k) {
        if (k >= steps->size()) {
            std::printf("selftest: done, %d failed\n", *failures);
            std::fflush(stdout);
            QCoreApplication::exit(*failures ? 1 : 0);
            return;
        }
        (*steps)[k].act();
        QTimer::singleShot((*steps)[k].wait, window, [=] {
            const bool ok = (*steps)[k].check();
            *failures += ok ? 0 : 1;
            // PINHOLE_SELFTEST_DIR: the window after each step, to look at.
            if (const QString dir = qEnvironmentVariable("PINHOLE_SELFTEST_DIR"); !dir.isEmpty())
                window->grabWindow().save(QStringLiteral("%1/mouse-%2.png").arg(dir).arg(k + 1, 2, 10, QLatin1Char('0')));
            std::printf("selftest: %-4s %s%s\n", ok ? "ok" : "FAIL", (*steps)[k].what,
                        ok ? "" : qPrintable(QStringLiteral(" (viewer %1 at start%2%3, offset %4; viewer presses %5, clicks %6)")
                                                 .arg(viewerPtr->isOpen() ? "open" : "closed")
                                                 .arg(viewerPtr->index() - *start >= 0 ? "+" : "")
                                                 .arg(viewerPtr->index() - *start)
                                                 .arg(viewerPtr->property("offset").toDouble(), 0, 'f', 0)
                                                 .arg(window->findChild<QObject *>("photoViewer")->property("presses").toInt())
                                                 .arg(window->findChild<QObject *>("photoViewer")->property("clicks").toInt())));
            std::fflush(stdout);
            (*run)(k + 1);
        });
    };
    QTimer::singleShot(1200, window, [run] { (*run)(0); });
}

void runLiveSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &folder) {
    QTimer::singleShot(1500, window, [=, &library, &viewer] {
        const int index = findIndex(library, QStringLiteral("a.is_live"));
        if (index < 0) {
            std::printf("selftest: no Live Photo in the library\n");
            QCoreApplication::quit();
            return;
        }
        viewer.openAt(index, QRectF(0, 0, 100, 100));
        auto *clock = new QElapsedTimer;
        auto state = [window, &viewer] {
            QObject *b = window->findChild<QObject *>(QStringLiteral("liveButton"));
            QObject *notice = window->findChild<QObject *>(QStringLiteral("liveNotice"));
            return QStringLiteral("loading %1, button %2%3, notice \"%4\"")
                .arg(viewer.liveLoading())
                .arg(b && b->property("active").toBool() ? "lit" : "plain")
                .arg(b && b->property("busy").toBool() ? " busy" : "")
                .arg(notice && notice->property("opacity").toReal() > 0 ? notice->property("text").toString() : QString());
        };
        QObject::connect(&viewer, &Viewer::liveFailed, window, [clock](const QString &message) {
            std::printf("selftest: %5lld ms: failed: %s\n", clock->elapsed(), qPrintable(message));
        });
        // The photo sharp first, as it would be by the time anyone presses.
        QTimer::singleShot(2500, window, [=] {
            clock->start();
            key(window, Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
            key(window, Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
            std::printf("selftest: pressed L twice: %s\n", qPrintable(state()));
            for (int ms : {100, 400, 1000, 2000, 3500, 5000, 8000})
                QTimer::singleShot(ms, window, [=] {
                    std::printf("selftest: %5lld ms: %s\n", clock->elapsed(), qPrintable(state()));
                    std::fflush(stdout);
                    // The top only: a whole window takes long enough to save
                    // that the timers and animations here fall behind.
                    const QImage frame = window->grabWindow();
                    frame.copy(0, 0, frame.width(), frame.height() / 8)
                        .save(QStringLiteral("%1/live-%2.png").arg(folder).arg(ms, 4, 10, QLatin1Char('0')));
                    if (ms == 8000)
                        QCoreApplication::quit();
                });
        });
    });
}

void runCopyTwiceSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const QString &folder) {
    QTimer::singleShot(1500, window, [=, &viewer] {
        const int index = grid->indexAt(grid->width() / 2, grid->height() / 2);
        viewer.openAt(index, grid->rectOf(index));
        auto phase = [window] {
            QObject *b = window->findChild<QObject *>(QStringLiteral("viewerCopy"));
            return b ? b->property("phase").toString() : QStringLiteral("?");
        };
        auto copyAndGrab = [=](int n) {
            // PINHOLE_SELFTEST_NOCOPY: show the tick without copying anything.
            if (qEnvironmentVariableIsSet("PINHOLE_SELFTEST_NOCOPY")) {
                QObject *b = window->findChild<QObject *>(QStringLiteral("viewerCopy"));
                b->setProperty("phase", "working");
                QMetaObject::invokeMethod(qmlContext(window->contentItem())->contextProperty("outgoing").value<QObject *>(),
                                          "copied", Q_ARG(int, 1), Q_ARG(int, 0));
                std::printf("selftest: tick %d without a copy: %s\n", n, qPrintable(phase()));
                return;
            }
            click(window, centreOf(window, "viewerCopy"));
            std::printf("selftest: click %d: right after, %s\n", n, qPrintable(phase()));
            QTimer::singleShot(150, window, [=] {
                std::printf("selftest: click %d: 150 ms on, %s\n", n, qPrintable(phase()));
                window->grabWindow().save(QStringLiteral("%1/copy-%2.png").arg(folder).arg(n));
                std::fflush(stdout);
            });
        };
        // Every change of the button's phase, with when.
        auto *clock = new QElapsedTimer;
        clock->start();
        auto last = std::make_shared<QString>();
        auto *watch = new QTimer(window);
        QObject::connect(watch, &QTimer::timeout, window, [=] {
            if (const QString p = phase(); p != *last) {
                std::printf("selftest:   %5lld ms: %s\n", clock->elapsed(), qPrintable(p));
                *last = p;
            }
        });
        watch->start(5);
        auto frames = std::make_shared<int>(0);
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [frames] { *frames += 1; }, Qt::QueuedConnection);
        for (int ms : {1000, 1600, 2500, 3500, 4900})
            QTimer::singleShot(ms, window, [=] {
                std::printf("selftest:   %5d ms: %d frames so far\n", ms, *frames);
            });
        QTimer::singleShot(1500, window, [=] { copyAndGrab(1); });
        QTimer::singleShot(5000, window, [=] {
            std::printf("selftest: before click 2, %s\n", qPrintable(phase()));
            copyAndGrab(2);
        });
        QTimer::singleShot(7000, window, [] {
            std::printf("selftest: done\n");
            std::fflush(stdout);
            QCoreApplication::quit();
        });
    });
}

void runDropSelfTest(QQuickWindow *window, const Library &library, Incoming &incoming, const QString &dir,
                     const QStringList &files) {
    // Once the service has said what the source takes.
    QTimer::singleShot(2000, window, [=, &library, &incoming] {
        auto *mime = new QMimeData;
        QList<QUrl> urls;
        for (const QString &f : files)
            urls.append(QUrl::fromLocalFile(f));
        mime->setUrls(urls);
        const QPoint middle(window->width() / 2, window->height() / 2);
        const int before = library.count();
        auto *clock = new QElapsedTimer;
        clock->start();
        QObject *target = window->findChild<QObject *>(QStringLiteral("dropTarget"));
        auto grab = [window, dir](const QString &name) { window->grabWindow().save(dir + '/' + name + ".png"); };

        QWindowSystemInterface::handleDrag(window, mime, middle, Qt::CopyAction | Qt::MoveAction, Qt::LeftButton, {});
        QTimer::singleShot(400, window, [=, &library, &incoming] {
            std::printf("selftest: hovering %lld files: lit %s, wants %d files and %d folders\n", qint64(files.size()),
                        target->property("lit").toBool() ? "yes" : "NO", target->property("files").toInt(),
                        target->property("folders").toInt());
            std::fflush(stdout);
            grab("1-hover");
            const auto response = QWindowSystemInterface::handleDrop(window, mime, middle, Qt::CopyAction | Qt::MoveAction,
                                                                     Qt::LeftButton, {});
            std::printf("selftest: dropped: accepted %s as %s\n", response.isAccepted() ? "yes" : "NO",
                        response.acceptedAction() == Qt::CopyAction ? "a copy" : "SOMETHING ELSE");
            std::fflush(stdout);
            QTimer::singleShot(150, window, [=] { grab("2-adding"); });
            auto *watch = new QTimer(window);
            auto said = std::make_shared<bool>(false);
            QObject::connect(watch, &QTimer::timeout, window, [=, &library, &incoming] {
                if (!incoming.busy() && !*said) {
                    *said = true;
                    std::printf("selftest: %5lld ms: done: \"%s\"; %s\n", clock->elapsed(), qPrintable(incoming.status()),
                                incoming.problems().isEmpty() ? "no problems"
                                                              : qPrintable(incoming.problemsTitle() + ": " + incoming.problems().join("; ")));
                    std::fflush(stdout);
                    grab("3-done");
                }
                if (*said && library.count() > before) {
                    watch->stop();
                    std::printf("selftest: %5lld ms: the grid has %d new (%d to %d)\n", clock->elapsed(), library.count() - before,
                                before, library.count());
                    std::fflush(stdout);
                    QTimer::singleShot(600, window, [=] {
                        grab("4-grid");
                        QCoreApplication::quit();
                    });
                } else if (clock->elapsed() > 120000) {
                    watch->stop();
                    std::printf("selftest: NOTHING new in the grid after two minutes\n");
                    std::fflush(stdout);
                    QCoreApplication::exit(1);
                }
            });
            watch->start(100);
        });
    });
}

void runFavouriteSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder) {
    QTimer::singleShot(2500, window, [=, &viewer, &library] {
        const int newest = library.count() - 1;
        if (newest < 2) {
            std::printf("selftest: too few photos\n");
            QCoreApplication::quit();
            return;
        }
        viewer.openAt(newest, grid->rectOf(newest));
        // Favourites among the newest `n`, by id, as a reload moves them.
        const QByteArray id = library.at(newest).id;
        auto favourites = [&library, id](int n) {
            int at = -1;
            for (int i = 0; i < library.count(); ++i)
                if (library.at(i).id == id)
                    at = i;
            int count = 0;
            for (int i = at; i > at - n && i >= 0; --i)
                count += library.at(i).favorite;
            return count;
        };
        auto grab = [window, folder](const QString &name, bool bottom) {
            const QImage frame = window->grabWindow();
            const int h = std::min(frame.height(), 200);
            frame.copy(0, bottom ? frame.height() - h : 0, frame.width(), h).save(QStringLiteral("%1/%2.png").arg(folder, name));
        };
        struct Step {
            QString name;
            bool bottom;
        };
        auto steps = std::make_shared<QList<Step>>(QList<Step>{
            {QStringLiteral("viewer-1"), false}, {QStringLiteral("viewer-2"), false},
            {QStringLiteral("select-1"), true}, {QStringLiteral("select-2"), true},
        });
        auto next = std::make_shared<std::function<void()>>();
        *next = [=, &viewer] {
            if (steps->isEmpty()) {
                QCoreApplication::quit();
                return;
            }
            const Step step = steps->takeFirst();
            const int n = step.bottom ? library.count() : 1;
            // Select mode once the viewer has gone (its own F goes with it).
            if (step.bottom && !grid->selecting()) {
                key(window, Qt::Key_Escape);
                grid->setSelecting(true);
                grid->selectAll();
                steps->prepend(step);
                QTimer::singleShot(1000, window, [next] { (*next)(); });
                return;
            }
            const int before = favourites(n);
            grab(step.name + QStringLiteral("-before"), step.bottom);
            key(window, Qt::Key_F);
            const int atOnce = favourites(n);
            QTimer::singleShot(1500, window, [=, &viewer] {
                std::printf("selftest: %s: favourites of %d %d, at once %d, after the service %d%s\n", qPrintable(step.name), n,
                            before, atOnce, favourites(n), step.bottom ? "" : viewer.isFavorite() ? " (viewer ♥)" : " (viewer ♡)");
                std::fflush(stdout);
                grab(step.name + QStringLiteral("-after"), step.bottom);
                (*next)();
            });
        };
        QTimer::singleShot(1200, window, [next] { (*next)(); });
    });
}

void runActionBarSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder) {
    // Late enough for the plugins to have stacked their series.
    QTimer::singleShot(4000, window, [=, &viewer, &library] {
        int index = -1;
        for (int i = library.count() - 1; i >= 0 && index < 0; --i)
            if (library.at(i).live && grid->stackOf(i) >= 0)
                index = i;
        if (index < 0) {
            std::printf("selftest: no Live Photo in a series\n");
            QCoreApplication::quit();
            return;
        }
        viewer.openAt(index, grid->rectOf(index));
        auto menuOpen = [window](const char *bar) {
            QObject *b = window->findChild<QObject *>(QString::fromLatin1(bar));
            return b && b->property("menuOpen").toBool();
        };
        struct Step {
            int key;
            Qt::KeyboardModifiers mods;
            QString name; // grabbed after the key, if any
            bool bottom;
        };
        // Each menu on its key, then Esc: the menu goes, the photo stays.
        auto steps = std::make_shared<QList<Step>>(QList<Step>{
            {0, Qt::NoModifier, QStringLiteral("viewer"), false},
            {Qt::Key_G, Qt::ShiftModifier, QStringLiteral("viewer-series"), false},
            {Qt::Key_Down, Qt::NoModifier, QStringLiteral("viewer-series-down"), false},
            {Qt::Key_Escape, Qt::NoModifier, {}, false},
            {Qt::Key_L, Qt::ShiftModifier, QStringLiteral("viewer-live"), false},
            {Qt::Key_Escape, Qt::NoModifier, {}, false},
            {Qt::Key_C, Qt::ShiftModifier, QStringLiteral("viewer-copy"), false},
            {Qt::Key_Escape, Qt::NoModifier, {}, false},
            {Qt::Key_Escape, Qt::NoModifier, {}, false},
            {Qt::Key_C, Qt::ShiftModifier, QStringLiteral("select-copy"), true},
            {Qt::Key_Escape, Qt::NoModifier, {}, true},
            {Qt::Key_G, Qt::ShiftModifier, QStringLiteral("select-series"), true},
        });
        auto next = std::make_shared<std::function<void()>>();
        *next = [=, &viewer] {
            if (steps->isEmpty()) {
                QCoreApplication::quit();
                return;
            }
            const Step step = steps->takeFirst();
            // Select mode once the viewer has gone.
            if (step.bottom && !grid->selecting()) {
                grid->setSelecting(true);
                grid->selectAll();
            }
            if (step.key != 0)
                key(window, step.key, step.mods);
            QTimer::singleShot(600, window, [=, &viewer] {
                std::printf("selftest: %s: viewer %s, viewer menu %s, select menu %s\n",
                            qPrintable(step.name.isEmpty() ? QStringLiteral("key") : step.name),
                            viewer.isOpen() ? "open" : "closed", menuOpen("viewerActions") ? "open" : "closed",
                            menuOpen("selectBar") ? "open" : "closed");
                std::fflush(stdout);
                if (!step.name.isEmpty()) {
                    const QImage frame = window->grabWindow();
                    const int h = std::min(frame.height(), 420);
                    frame.copy(0, step.bottom ? frame.height() - h : 0, frame.width(), h)
                        .save(QStringLiteral("%1/%2.png").arg(folder, step.name));
                }
                (*next)();
            });
        };
        QTimer::singleShot(1500, window, [next] { (*next)(); });
    });
}

void runTourSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder) {
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto tour = [window] { return window->findChild<QObject *>(QStringLiteral("tour")); };
    auto grab = [window, folder](const QString &name) { window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name)); };
    auto describe = [tour, &viewer](const QString &name) {
        QObject *t = tour();
        return QStringLiteral("%1: tour %2, step %3 of %4, lit %5, viewer %6")
            .arg(name, t->property("shown").toBool() ? "shown" : "gone")
            .arg(t->property("index").toInt() + 1)
            .arg(t->property("steps").toList().size())
            .arg(t->property("lit").toBool() ? "yes" : "no", viewer.isOpen() ? "open" : "closed");
    };
    auto wait = std::make_shared<QTimer>();
    wait->setInterval(250);
    QObject::connect(wait.get(), &QTimer::timeout, window, [=, &viewer] {
        // A source signed out (as a test profile's is) says so first, and
        // no tour shows over that: looked past, as Esc does.
        window->setProperty("troubleSeen", true);
        QObject *t = tour();
        if (!t || !t->property("shown").toBool()) {
            if (clock->elapsed() > 60000) {
                say(QStringLiteral("no tour showed by itself in 60 s"));
                QCoreApplication::quit();
            }
            return;
        }
        wait->stop();
        say(QStringLiteral("a tour showed by itself after %1 s").arg(clock->elapsed() / 1000.0, 0, 'f', 1));
        const int count = t->property("steps").toList().size();
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [=, &viewer](int k) {
            QTimer::singleShot(1600, window, [=, &viewer] {
                say(describe(QStringLiteral("step %1").arg(k + 1)));
                grab(QStringLiteral("step-%1").arg(k + 1));
                key(window, Qt::Key_Right);
                if (k + 1 < count) {
                    (*step)(k + 1);
                    return;
                }
                QTimer::singleShot(1200, window, [=, &viewer] {
                    say(describe(QStringLiteral("after the last")));
                    // As the side panel's button asks for it.
                    QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("sidePanel")), "tourRequested",
                                              Q_ARG(QString, QStringLiteral("pinhole.series")));
                    QTimer::singleShot(1600, window, [=, &viewer] {
                        say(describe(QStringLiteral("again")));
                        grab(QStringLiteral("again"));
                        key(window, Qt::Key_Escape);
                        // Long enough for it to come back by itself, if it would.
                        QTimer::singleShot(3000, window, [=, &viewer] {
                            say(describe(QStringLiteral("after Esc")));
                            QCoreApplication::quit();
                        });
                    });
                });
            });
        };
        (*step)(0);
    });
    wait->start();
}

void runSeriesSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder) {
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    // In a plugin, by the items it shows: a Loader's item is not its child.
    auto editor = [window] {
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("seriesEditor"))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(child))
                    return found;
            return nullptr;
        };
        return static_cast<QObject *>(find(window->contentItem()));
    };
    auto grab = [window, folder](const QString &name) { window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name)); };
    auto describe = [editor](const QString &name) {
        QObject *e = editor();
        return QStringLiteral("%1: %2, on %3 of %4 in the middle, looking %5")
            .arg(name, e->property("shown").toBool() ? "shown" : "gone")
            .arg(e->property("flowAt").toInt() + 1)
            .arg(e->property("flow").toList().size())
            .arg(e->property("looking").toBool() ? "yes" : "no");
    };
    // As the system sends it, to reach Qt Quick's handlers.
    auto wheel = [window](int delta) {
        const QPointF at(window->width() / 2.0, window->height() / 2.0);
        QWindowSystemInterface::handleWheelEvent(window, at, window->mapToGlobal(at), QPoint(0, delta / 8), QPoint(0, delta));
    };
    auto wait = std::make_shared<QTimer>();
    wait->setInterval(250);
    QObject::connect(wait.get(), &QTimer::timeout, window, [=, &viewer] {
        window->setProperty("troubleSeen", true);
        QObject *e = editor();
        QObject *plugin = e ? e->property("plugin").value<QObject *>() : nullptr;
        const QVariantList series = plugin ? plugin->property("series").toList() : QVariantList();
        if (series.isEmpty()) {
            if (clock->elapsed() > 60000) {
                say(QStringLiteral("no series found in 60 s"));
                QCoreApplication::quit();
            }
            return;
        }
        wait->stop();
        int longest = 0;
        for (int k = 1; k < series.size(); ++k)
            if (series[k].toList().size() > series[longest].toList().size())
                longest = k;
        say(QStringLiteral("%1 series, the longest of %2").arg(series.size()).arg(series[longest].toList().size()));
        // As G on a photo of it does: over the viewer, which leaves the
        // keys and the wheel to it.
        const int first = series[longest].toList().first().toInt();
        viewer.openAt(first, QRectF(0, 0, 100, 100));
        QMetaObject::invokeMethod(e, "openSeries", Q_ARG(QVariant, longest), Q_ARG(QVariant, first));
        // Each step: what to do, then a moment for it to slide and sharpen.
        const QList<std::pair<QString, std::function<void()>>> steps = {
            { QStringLiteral("open"), [] {} },
            { QStringLiteral("right-1"), [window] { key(window, Qt::Key_Right); } },
            { QStringLiteral("right-2"), [window] { key(window, Qt::Key_Right); } },
            { QStringLiteral("right-3"), [window] { key(window, Qt::Key_Right); } },
            { QStringLiteral("wheel"), [wheel] { wheel(-120); wheel(-120); } },
            { QStringLiteral("end"), [window] { key(window, Qt::Key_End); } },
            { QStringLiteral("close-up"), [window] { key(window, Qt::Key_Space); } },
            { QStringLiteral("home"), [window] { key(window, Qt::Key_Space); key(window, Qt::Key_Home); } },
        };
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [=, &viewer](int n) {
            if (n == steps.size()) {
                key(window, Qt::Key_Escape);
                QTimer::singleShot(600, window, [=, &viewer] {
                    say(describe(QStringLiteral("after Esc")) + QStringLiteral(", the viewer %1 at %2 and %3")
                        .arg(viewer.isOpen() ? "open" : "closed").arg(viewer.property("index").toInt() == first ? "the photo it opened on" : "another")
                        .arg(viewer.zoomed() ? "zoomed" : "not zoomed"));
                    QCoreApplication::quit();
                });
                return;
            }
            steps[n].second();
            QTimer::singleShot(1500, window, [=] {
                say(describe(steps[n].first));
                grab(steps[n].first);
                (*step)(n + 1);
            });
        };
        (*step)(0);
    });
    wait->start();
}

void runUndoSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder) {
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto editor = [window] {
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("seriesEditor"))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(child))
                    return found;
            return nullptr;
        };
        return static_cast<QObject *>(find(window->contentItem()));
    };
    auto grab = [window, folder](const QString &name) { window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name)); };
    auto wait = std::make_shared<QTimer>();
    wait->setInterval(250);
    QObject::connect(wait.get(), &QTimer::timeout, window, [=, &viewer] {
        window->setProperty("troubleSeen", true);
        QObject *e = editor();
        QObject *plugin = e ? e->property("plugin").value<QObject *>() : nullptr;
        const QVariantList series = plugin ? plugin->property("series").toList() : QVariantList();
        if (series.isEmpty()) {
            if (clock->elapsed() > 60000) {
                say(QStringLiteral("no series found in 60 s"));
                QCoreApplication::exit(1);
            }
            return;
        }
        wait->stop();
        int longest = 0;
        for (int k = 1; k < series.size(); ++k)
            if (series[k].toList().size() > series[longest].toList().size())
                longest = k;
        const int first = series[longest].toList().first().toInt();
        viewer.openAt(first, QRectF(0, 0, 100, 100));
        QMetaObject::invokeMethod(e, "openSeries", Q_ARG(QVariant, longest), Q_ARG(QVariant, first));
        const int whole = e->property("flow").toList().size();
        say(QStringLiteral("a series of %1 open to put right; the keys %2").arg(whole)
            .arg(window->property("historyKeys").toBool() ? "on" : "off"));
        // Each: the keys, and how many photos the series should then have.
        const QList<std::tuple<QString, std::function<void()>, int>> steps = {
            { QStringLiteral("take-out"), [window] { key(window, Qt::Key_X); }, whole - 1 },
            { QStringLiteral("undo"), [window] { key(window, Qt::Key_Z, Qt::ControlModifier); }, whole },
            { QStringLiteral("redo"), [window] { key(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); }, whole - 1 },
            { QStringLiteral("undo-again"), [window] { key(window, Qt::Key_Z, Qt::ControlModifier); }, whole },
            { QStringLiteral("nothing-to-undo"), [window] { key(window, Qt::Key_Z, Qt::ControlModifier); }, whole },
        };
        auto failed = std::make_shared<bool>(false);
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [=](int n) {
            if (n == steps.size()) {
                say(*failed ? QStringLiteral("FAILED") : QStringLiteral("ok"));
                QCoreApplication::exit(*failed ? 1 : 0);
                return;
            }
            const auto &[name, press, wanted] = steps[n];
            press();
            QTimer::singleShot(600, window, [=] {
                const auto &[name, press, wanted] = steps[n];
                const int now = editor()->property("flow").toList().size();
                auto *notice = window->findChild<QQuickItem *>(QStringLiteral("historyNotice"));
                say(QStringLiteral("%1: %2 photos (%3)%4").arg(name).arg(now).arg(now == wanted ? "as it should" : "WRONG")
                    .arg(notice && notice->opacity() > 0 ? QStringLiteral(", saying \"%1\"").arg(notice->property("text").toString()) : QString()));
                *failed = *failed || now != wanted;
                grab(name);
                (*step)(n + 1);
            });
        };
        (*step)(0);
    });
    wait->start();
}
