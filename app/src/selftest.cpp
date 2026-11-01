#include "selftest.h"

#include "keymap.h"

#include "incoming.h"
#include "library.h"
#include "libraryreloader.h"
#include "outgoing.h"
#include "photogrid.h"
#include "photoprovider.h"
#include "plugins.h"
#include "viewer.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeData>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
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

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

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

namespace {

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
    // Kinds that did not come within a minute, and pictures that do not
    // decode: the run fails.
    auto failures = std::make_shared<int>(0);
    *next = [&library, &viewer, cases, next, failures](int k) {
        if (k >= cases.size()) {
            std::printf("selftest: done, %d failed\n", *failures);
            std::fflush(stdout);
            QCoreApplication::exit(*failures ? 1 : 0);
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
        // Once, whichever comes first: the last stage, or the minute.
        auto finished = std::make_shared<bool>(false);
        auto finish = [connections, next, k, finished] {
            if (std::exchange(*finished, true))
                return;
            for (const auto &c : *connections)
                QObject::disconnect(c);
            QTimer::singleShot(0, [next, k] { (*next)(k + 1); });
        };
        // A picture shown that does not decode fails the run too.
        auto report = [clock, failures](const char *name, const char *what, const QString &file) {
            QElapsedTimer decode;
            decode.start();
            const QImage image = file.isEmpty() ? QImage() : decodePhoto(file, QSize());
            if (!file.isEmpty() && image.isNull())
                ++*failures;
            std::printf("selftest: %-11s %-9s after %5lld ms%s\n", name, what, clock->elapsed(),
                        file.isEmpty()     ? ""
                        : image.isNull()   ? qPrintable(QStringLiteral("; DOES NOT DECODE: %1").arg(file))
                                           : qPrintable(QStringLiteral("; decodes to %1x%2 in %3 ms")
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
        QTimer::singleShot(60000, &viewer, [c, finish, failures, finished] {
            if (*finished)
                return;
            std::printf("selftest: %s: timed out\n", c.name);
            ++*failures;
            finish();
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
        // The plugins' marks each photo carries, by its id: the same after
        // (those of photos followed until given again, of fields read
        // again). Pinhole's own stack mark aside: a stack may take in the
        // photo that came.
        auto plugins = [grid, &library] {
            std::map<QByteArray, QStringList> marks;
            int marked = 0;
            for (int i = 0; i < library.count(); ++i) {
                QStringList ids = grid->marksOn(i);
                ids.removeAll(QStringLiteral("pinhole/stack"));
                marked += !ids.isEmpty();
                marks[library.at(i).id] = ids;
            }
            return std::make_pair(marks, marked);
        };
        const auto marksBefore = plugins();
        std::printf("selftest: %d items; viewer on %s; %d photos with marks; waiting for the service\n", before,
                    open.constData(), marksBefore.second);
        std::fflush(stdout);
        QObject::connect(grid, &PhotoGrid::libraryChanged, grid, [&library, grid, &viewer, before, open, b, plugins, marksBefore] {
            QTimer::singleShot(500, grid, [&library, grid, &viewer, before, open, b, plugins, marksBefore] {
                const auto marksAfter = plugins();
                int same = 0, moved = 0;
                for (const auto &[id, marks] : marksAfter.first) {
                    const auto was = marksBefore.first.find(id);
                    if (was == marksBefore.first.end())
                        continue;
                    if (was->second == marks)
                        ++same;
                    else if (++moved <= 5)
                        std::printf("selftest: marks of %s were %s, now %s\n", id.constData(),
                                    qPrintable(was->second.join(',')), qPrintable(marks.join(',')));
                }
                std::printf("selftest: marks followed their photos: %s (%d the same, %d not; %d photos with marks)\n",
                            moved == 0 ? "yes" : "no", same, moved, marksAfter.second);
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
        // The keyboard's cursor: the first press shows it, the next moves it
        // a row down (where it is in the library, not on screen: the view
        // may glide after it).
        {"Down shows the cursor, and again moves it a row", [=] {
             grid->jumpToFraction(0.5);
             QTimer::singleShot(100, window, [=] {
                 key(window, Qt::Key_Down);
                 const int index = grid->currentIndex();
                 *mark = grid->rectOf(index).top() + grid->scrollFraction() * grid->scrollRange();
                 QTimer::singleShot(300, window, [=] { key(window, Qt::Key_Down); });
             });
         },
         [=] {
             const double at = grid->rectOf(grid->currentIndex()).top() + grid->scrollFraction() * grid->scrollRange();
             return grid->cursorShown() && std::abs(at - *mark - (grid->rectOf(1).left() - grid->rectOf(0).left())) < 2;
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
    auto card = [window] { return window->findChild<QObject *>(QStringLiteral("newPlugins")); };
    QObject *plugins = qmlEngine(window)->rootContext()->contextProperty("plugins").value<QObject *>();
    auto toured = [plugins](const QString &id) {
        QVariantMap info;
        QMetaObject::invokeMethod(plugins, "at", Q_RETURN_ARG(QVariantMap, info), Q_ARG(QString, id));
        return info.value("toured").toBool();
    };
    auto ready = [window](const QString &id) {
        QVariant item;
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, item), Q_ARG(QVariant, id));
        QObject *plugin = item.value<QObject *>();
        return plugin && plugin->property("tourReady").toBool();
    };
    auto failures = std::make_shared<int>(0);
    auto check = [say, failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    auto exitTest = [say, failures] {
        say(QStringLiteral("done, %1 failed").arg(*failures));
        QCoreApplication::exit(*failures ? 1 : 0);
    };
    // PINHOLE_TOUR_PLUGIN=<id>: that plugin's tour alone, as the side panel
    // starts it, each step waited on until its light has landed on the
    // button it names (Info's map shows only once its photo's place is
    // read), how long that took said, and when the button showed.
    // PINHOLE_TOUR_DWELL: ms on each step at least, as one reading it.
    if (const QString only = qEnvironmentVariable("PINHOLE_TOUR_PLUGIN"); !only.isEmpty()) {
        auto poll = std::make_shared<QTimer>();
        poll->setInterval(50);
        auto stepClock = std::make_shared<QElapsedTimer>();
        auto at = std::make_shared<int>(-1);
        auto shownAfter = std::make_shared<qint64>(-1), landedAfter = std::make_shared<qint64>(-1);
        const qint64 dwell = qEnvironmentVariableIntValue("PINHOLE_TOUR_DWELL");
        QObject::connect(poll.get(), &QTimer::timeout, window, [=, &viewer] {
            window->setProperty("troubleSeen", true);
            QObject *t = tour(), *c = card();
            if (*at < 0) {
                // Whatever showed by itself, looked past; then its tour.
                if (clock->elapsed() < 4000 || !ready(only))
                    return;
                if ((t && t->property("shown").toBool()) || (c && c->property("shown").toBool())) {
                    key(window, Qt::Key_Escape);
                    return;
                }
                QMetaObject::invokeMethod(window, "startTour", Q_ARG(QVariant, only));
                *at = 0;
                stepClock->start();
                return;
            }
            const int steps = t->property("steps").toList().size();
            auto *tourItem = qobject_cast<QQuickItem *>(t);
            QObject *action = t->property("action").value<QObject *>();
            auto *button = action ? action->property("button").value<QQuickItem *>() : nullptr;
            const qreal pad = t->property("pad").toReal();
            const QRectF hole(t->property("hx").toReal(), t->property("hy").toReal(), t->property("hw").toReal(), t->property("hh").toReal());
            // What the step says it lights, first of: its settings (the
            // plugin's part of the side panel), its action's button, its
            // photo's card in the grid (the viewer not over it); else
            // nothing, and then the light is closed.
            const QVariant stepValue = t->property("step");
            const QVariant stepMap = stepValue.metaType() == QMetaType::fromType<QJSValue>() ? stepValue.value<QJSValue>().toVariant() : stepValue;
            const bool settings = stepMap.toMap().value(QStringLiteral("settings")).toBool();
            const int photo = t->property("photo").toInt();
            auto *grid = t->property("grid").value<QQuickItem *>();
            QObject *photoViewer = t->property("photoViewer").value<QObject *>();
            const bool covered = photoViewer && photoViewer->property("shown").toBool();
            QString what, where;
            QRectF want;
            if (settings) {
                what = QStringLiteral("its settings");
                auto *part = t->property("panelPart").value<QQuickItem *>();
                where = part ? QStringLiteral("the side panel") : QStringLiteral("not in the side panel");
                if (part && part->isVisible() && part->width() > 0)
                    want = QRectF(part->mapToItem(tourItem, QPointF(0, 0)), part->size());
            } else if (action) {
                what = action->property("text").toString();
                if (what.isEmpty())
                    what = QStringLiteral("its action");
                where = !button ? QStringLiteral("no button") : button->objectName().isEmpty() ? QStringLiteral("its button") : button->objectName();
                if (button && button->isVisible() && button->width() > 0)
                    want = QRectF(button->mapToItem(tourItem, QPointF(0, 0)), button->size());
            } else if (photo >= 0) {
                what = QStringLiteral("photo %1").arg(photo);
                where = covered ? QStringLiteral("under the viewer") : QStringLiteral("the grid");
                QRectF card;
                if (grid)
                    QMetaObject::invokeMethod(grid, "rectOf", Q_RETURN_ARG(QRectF, card), Q_ARG(int, photo));
                if (grid && !covered && card.width() > 0)
                    want = grid->mapRectToItem(tourItem, card);
            } else {
                what = QStringLiteral("nothing");
                where = QStringLiteral("no light");
            }
            if (!want.isNull()) {
                want.adjust(-pad, -pad, pad, pad);
                if (*shownAfter < 0)
                    *shownAfter = stepClock->elapsed();
            }
            const bool lit = t->property("lit").toBool();
            const bool landed = what == QLatin1String("nothing")
                                    ? !lit && hole.width() + hole.height() < 2
                                    : lit && !want.isNull()
                                          && std::abs(hole.x() - want.x()) + std::abs(hole.y() - want.y())
                                                     + std::abs(hole.width() - want.width()) + std::abs(hole.height() - want.height()) < 2;
            if (landed && *landedAfter < 0)
                *landedAfter = stepClock->elapsed();
            if ((!landed && stepClock->elapsed() < 10000) || stepClock->elapsed() < dwell)
                return;
            check(landed, QStringLiteral("step %1 of %2: the light on %3 (%4), landed after %5 ms, what it lights there after %6 ms, viewer %7")
                              .arg(*at + 1).arg(steps).arg(what, where)
                              .arg(*landedAfter).arg(*shownAfter).arg(viewer.isOpen() ? "open" : "closed"));
            *shownAfter = *landedAfter = -1;
            grab(QStringLiteral("%1-step-%2").arg(only).arg(*at + 1));
            if (++*at < steps) {
                key(window, Qt::Key_Right);
                stepClock->restart();
                return;
            }
            poll->stop();
            key(window, Qt::Key_Escape);
            QTimer::singleShot(1000, window, [=] {
                say(describe(QStringLiteral("after the last")));
                exitTest();
            });
        });
        poll->start();
        return;
    }
    // Calls `then` once `ready` holds, polled; `late` after `seconds`.
    auto when = [window](std::function<bool()> ready, int seconds, std::function<void()> then, std::function<void()> late) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto poll = new QTimer(window);
        poll->setInterval(250);
        QObject::connect(poll, &QTimer::timeout, window, [=] {
            window->setProperty("troubleSeen", true);
            if (ready()) {
                poll->deleteLater();
                then();
            } else if (clock->elapsed() > seconds * 1000) {
                poll->deleteLater();
                late();
            }
        });
        poll->start();
    };
    auto shown = [](QObject *o) { return o && o->property("shown").toBool(); };
    // However soon a tour that opens a photo ends, the viewer it opened
    // closes and one open before stays: Esc as it starts (Background's first
    // step opens a photo) and mid-open, between steps, as a step without a
    // photo closes it, and back on a photo mid-close. Then the test ends.
    auto photoViewer = [window] { return window->findChild<QObject *>(QStringLiteral("photoViewer")); };
    auto viewerShown = [photoViewer] {
        QObject *p = photoViewer();
        return p && p->property("shown").toBool();
    };
    auto state = [=, &viewer] {
        QObject *p = photoViewer();
        return QStringLiteral("tour %1, step %2, viewer %3 on %4, shown %5 (progress %6)")
            .arg(shown(tour()) ? "shown" : "gone")
            .arg(tour()->property("index").toInt() + 1)
            .arg(viewer.isOpen() ? "open" : "closed")
            .arg(viewer.property("index").toInt())
            .arg(viewerShown() ? "yes" : "no")
            .arg(p ? p->property("progress").toReal() : -1, 0, 'f', 2);
    };
    const QString background = QStringLiteral("pinhole.background");
    auto startTour = [=, &viewer](const QString &what) {
        QMetaObject::invokeMethod(window, "startTour", Q_ARG(QVariant, background));
        const bool ok = shown(tour()) && viewer.isOpen();
        if (!ok)
            check(false, QStringLiteral("%1: Background's tour opens a photo as it starts (%2)").arg(what, state()));
        return ok;
    };
    // Keys at so many ms from now; 0 at once.
    auto keysAt = [window](const QList<QPair<int, int>> &keys) {
        for (const auto &[ms, code] : keys) {
            if (ms == 0)
                key(window, code);
            else
                QTimer::singleShot(ms, window, [window, code] { key(window, code); });
        }
    };
    auto closedAt = [=, &viewer](int ms, const QString &what, std::function<void()> next) {
        QTimer::singleShot(ms, window, [=, &viewer] {
            check(!shown(tour()) && !viewer.isOpen() && !viewerShown()
                      && !shown(window->findChild<QObject *>(QStringLiteral("sidePanel"))),
                  QStringLiteral("%1: the tour, the viewer and the side panel closed (%2)").arg(what, state()));
            next();
        });
    };
    using Case = std::function<void(std::function<void()>)>;
    auto cases = std::make_shared<QList<Case>>();
    for (const int ms : {0, 50, 150, 250, 500})
        cases->append([=](std::function<void()> next) {
            const QString what = QStringLiteral("Esc %1 ms after the tour opens a photo").arg(ms);
            if (!startTour(what))
                return next();
            keysAt({{ms, Qt::Key_Escape}});
            closedAt(ms + 1200, what, next);
        });
    cases->append([=](std::function<void()> next) {
        const QString what = QStringLiteral("→ at 100 ms, Esc 30 ms after (another photo opening)");
        if (!startTour(what))
            return next();
        keysAt({{100, Qt::Key_Right}, {130, Qt::Key_Escape}});
        closedAt(1400, what, next);
    });
    cases->append([=](std::function<void()> next) {
        const QString what = QStringLiteral("→ → at 100 and 200 ms to the step without a photo, Esc at 250 ms");
        if (!startTour(what))
            return next();
        keysAt({{100, Qt::Key_Right}, {200, Qt::Key_Right}, {250, Qt::Key_Escape}});
        closedAt(1500, what, next);
    });
    cases->append([=, &viewer](std::function<void()> next) {
        const QString what = QStringLiteral("→ → at 300 and 600 ms, ← at 650 ms, back on a photo as the viewer closes");
        if (!startTour(what))
            return next();
        keysAt({{300, Qt::Key_Right}, {600, Qt::Key_Right}, {650, Qt::Key_Left}});
        QTimer::singleShot(1700, window, [=, &viewer] {
            QObject *p = photoViewer();
            check(shown(tour()) && tour()->property("index").toInt() == 1 && viewer.isOpen() && p
                      && p->property("progress").toReal() == 1,
                  QStringLiteral("%1: the viewer open again (%2)").arg(what, state()));
            keysAt({{0, Qt::Key_Escape}});
            closedAt(1200, what + ", then Esc", next);
        });
    });
    // A photo open before the tour, as one had it.
    auto ownPhoto = std::make_shared<int>(-1);
    auto openOwn = [=, &viewer](std::function<void()> then) {
        *ownPhoto = viewer.count() / 2;
        viewer.openAt(*ownPhoto, QRectF(0, 0, 100, 100));
        QTimer::singleShot(1000, window, then);
    };
    cases->append([=, &viewer](std::function<void()> next) {
        openOwn([=, &viewer] {
            const QString what = QStringLiteral("a photo open before the tour, Esc at 50 ms");
            if (!startTour(what))
                return next();
            keysAt({{50, Qt::Key_Escape}});
            QTimer::singleShot(1250, window, [=, &viewer] {
                QObject *p = photoViewer();
                check(!shown(tour()) && viewer.isOpen() && viewer.property("index").toInt() == *ownPhoto && p
                          && p->property("progress").toReal() == 1,
                      QStringLiteral("%1: it stays open on photo %2 (%3)").arg(what).arg(*ownPhoto).arg(state()));
                QMetaObject::invokeMethod(p, "closeViewer");
                next();
            });
        });
    });
    cases->append([=, &viewer](std::function<void()> next) {
        openOwn([=, &viewer] {
            QMetaObject::invokeMethod(photoViewer(), "closeViewer");
            QTimer::singleShot(60, window, [=, &viewer] {
                const QString what = QStringLiteral("a tour started as the viewer closes");
                if (!startTour(what))
                    return next();
                QTimer::singleShot(1000, window, [=, &viewer] {
                    QObject *p = photoViewer();
                    check(shown(tour()) && viewer.isOpen() && p && p->property("progress").toReal() == 1,
                          QStringLiteral("%1: the viewer open again for its step (%2)").arg(what, state()));
                    keysAt({{0, Qt::Key_Escape}});
                    closedAt(1200, what + ", then Esc", next);
                });
            });
        });
    });
    auto finish = [=, &viewer] {
        say(QStringLiteral("a tour that opens a photo, ended at once"));
        if (!ready(background)) {
            check(false, QStringLiteral("Background's tour, to end at once, ready"));
            return exitTest();
        }
        auto run = std::make_shared<std::function<void(int)>>();
        *run = [=, &viewer](int k) {
            if (k >= cases->size())
                return exitTest();
            // Nothing left open by the one before; what was, closed, to go on.
            when([=, &viewer] { return !shown(tour()) && !viewer.isOpen() && !viewerShown(); }, 3,
                 [=] { (*cases)[k]([=] { (*run)(k + 1); }); },
                 [=] {
                     say(QStringLiteral("left open, closed to go on (%1)").arg(state()));
                     QMetaObject::invokeMethod(photoViewer(), "closeViewer");
                     QTimer::singleShot(1000, window, [=] { (*cases)[k]([=] { (*run)(k + 1); }); });
                 });
        };
        (*run)(0);
    };
    // As the next launch would: the one by itself not yet had.
    auto relaunch = [window] {
        window->setProperty("touredThisLaunch", false);
        QMetaObject::invokeMethod(window, "collectPlugins", Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant()));
    };
    // The plugins the card listed; the one shown from it, and the one
    // turned on again.
    auto listed = std::make_shared<QStringList>();
    auto picked = std::make_shared<QString>();
    auto turned = std::make_shared<QString>();
    // Whose of the listed are seen: `seen` alone.
    auto onlySeen = [=](const QStringList &seen, const QString &what) {
        QStringList wrong;
        for (const QString &id : *listed)
            if (toured(id) != seen.contains(id))
                wrong << id + (toured(id) ? " seen" : " not seen");
        check(wrong.isEmpty(), QStringLiteral("%1%2").arg(what, wrong.isEmpty() ? QString() : ": " + wrong.join(", ")));
    };
    // The card as it shows: more than one, each ready.
    auto looked = [=](const QString &name) {
        const QStringList ids = card()->property("ids").toStringList();
        QStringList unready;
        for (const QString &id : ids)
            if (!ready(id))
                unready << id;
        say(QStringLiteral("%1 after %2 s: %3").arg(name).arg(clock->elapsed() / 1000.0, 0, 'f', 1).arg(ids.join(", ")));
        check(ids.size() > 1, QStringLiteral("%1 lists more than one").arg(name));
        check(unready.isEmpty(), QStringLiteral("%1 lists only those whose tour is ready%2")
                                     .arg(name, unready.isEmpty() ? QString() : ": not " + unready.join(", ")));
        return ids;
    };
    // The last: Skip all marks all the card lists seen.
    auto skipAll = [=] {
        relaunch();
        when([=] { return shown(card()) || shown(tour()); }, 30, [=] {
            if (!shown(card())) {
                say(QStringLiteral("one left, its tour by itself: %1").arg(tour()->property("pluginId").toString()));
                key(window, Qt::Key_Escape);
                return QTimer::singleShot(1000, window, finish);
            }
            const QStringList ids = looked(QStringLiteral("the card at the next launch"));
            grab(QStringLiteral("card-skip"));
            QMetaObject::invokeMethod(card(), "skipAll");
            QTimer::singleShot(1500, window, [=] {
                QStringList unseen;
                for (const QString &id : ids)
                    if (!toured(id))
                        unseen << id;
                check(!shown(card()) && !shown(tour()) && unseen.isEmpty(),
                      QStringLiteral("Skip all closes the card and marks them all seen%1")
                          .arg(unseen.isEmpty() ? QString() : ": not " + unseen.join(", ")));
                finish();
            });
        }, [=] {
            check(false, QStringLiteral("the card again at the next launch, for Skip all (tour free %1, keys %2's)")
                             .arg(window->property("tourFree").toBool() ? "yes" : "no", window->property("keyLayer").toString()));
            finish();
        });
    };
    // A plugin turned on later in the launch shows its tour by itself,
    // though the launch's has shown.
    auto turnOn = [=, &viewer] {
        for (const QString &id : *listed)
            if (id != *picked && !toured(id)) {
                *turned = id;
                break;
            }
        if (turned->isEmpty())
            return skipAll();
        QMetaObject::invokeMethod(plugins, "setEnabled", Q_ARG(QString, *turned), Q_ARG(bool, false));
        QTimer::singleShot(1500, window, [=, &viewer] {
            QMetaObject::invokeMethod(plugins, "setEnabled", Q_ARG(QString, *turned), Q_ARG(bool, true));
            when([=] { return shown(tour()) || shown(card()); }, 40, [=, &viewer] {
                check(shown(tour()) && tour()->property("pluginId").toString() == *turned && !shown(card()),
                      QStringLiteral("%1 turned off and on again: its tour by itself").arg(*turned));
                // As one reading it, then Esc.
                QTimer::singleShot(1600, window, [=, &viewer] {
                    say(describe(QStringLiteral("%1's tour").arg(*turned)));
                    grab(QStringLiteral("turned-on"));
                    key(window, Qt::Key_Escape);
                    QTimer::singleShot(1500, window, [=, &viewer] {
                        say(describe(QStringLiteral("after Esc")));
                        check(toured(*turned) && !shown(tour()) && !shown(card()) && !viewer.isOpen(),
                              QStringLiteral("%1 seen after Esc, nothing follows, the viewer it opened closed").arg(*turned));
                        skipAll();
                    });
                });
            }, [=] {
                check(false, QStringLiteral("%1 turned off and on again: no tour by itself in 40 s").arg(*turned));
                skipAll();
            });
        });
    };
    // Through the tour shown, then: none follows, only it is seen; Series'
    // again, Esc, and none follows.
    auto walk = [=, &viewer] {
        QObject *t = tour();
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
                // Long enough for another to come by itself, if one would.
                QTimer::singleShot(3000, window, [=, &viewer] {
                    say(describe(QStringLiteral("after the last")));
                    check(!shown(tour()) && !shown(card()), QStringLiteral("no tour, nor the card, follows the last step"));
                    check(!viewer.isOpen(), QStringLiteral("the viewer the tour opened is closed"));
                    if (!listed->isEmpty())
                        onlySeen({*picked}, QStringLiteral("of the card's, %1 alone is seen").arg(*picked));
                    // As the side panel's button asks for it.
                    QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("sidePanel")), "tourRequested",
                                              Q_ARG(QString, QStringLiteral("pinhole.series")));
                    QTimer::singleShot(1600, window, [=, &viewer] {
                        say(describe(QStringLiteral("again")));
                        check(shown(tour()) && tour()->property("pluginId").toString() == "pinhole.series",
                              QStringLiteral("Series' tour again from the side panel"));
                        grab(QStringLiteral("again"));
                        key(window, Qt::Key_Escape);
                        QTimer::singleShot(3000, window, [=, &viewer] {
                            say(describe(QStringLiteral("after Esc")));
                            check(!shown(tour()) && !shown(card()), QStringLiteral("no tour follows Esc"));
                            if (listed->isEmpty())
                                return finish();
                            turnOn();
                        });
                    });
                });
            });
        };
        (*step)(0);
    };
    // ⏎ on Series' row (or, after 30 s, the first ready): its tour.
    auto show = [=] {
        when([=] {
            const QStringList ids = card()->property("ids").toStringList();
            *picked = ids.contains(QStringLiteral("pinhole.series")) && ready(QStringLiteral("pinhole.series"))
                          ? QStringLiteral("pinhole.series") : clock->elapsed() > 30000 && !ids.isEmpty() ? ids.first() : QString();
            return !picked->isEmpty();
        }, 90, [=] {
            card()->setProperty("current", card()->property("ids").toStringList().indexOf(*picked));
            key(window, Qt::Key_Return);
            QTimer::singleShot(300, window, [=] {
                check(shown(tour()) && tour()->property("pluginId").toString() == *picked && !shown(card()),
                      QStringLiteral("⏎ on %1 shows its tour in the card's place").arg(*picked));
                if (!shown(tour()))
                    return finish();
                walk();
            });
        }, [=] {
            check(false, QStringLiteral("no tour on the card ready in 90 s"));
            finish();
        });
    };
    // A source signed out (as a test profile's is) says so first, and no
    // tour shows over that: looked past, as Esc does (`when`).
    when([=] { return shown(tour()) || shown(card()); }, 60, [=] {
        if (shown(tour())) {
            say(QStringLiteral("a tour showed by itself after %1 s: %2")
                    .arg(clock->elapsed() / 1000.0, 0, 'f', 1)
                    .arg(tour()->property("pluginId").toString()));
            return walk();
        }
        // The card: Later (Esc) leaves them all unseen, and nothing
        // follows; at the next launch it comes back.
        *listed = looked(QStringLiteral("the New plugins card showed by itself"));
        QTimer::singleShot(600, window, [=] {
            grab(QStringLiteral("card"));
            key(window, Qt::Key_Escape);
            QTimer::singleShot(3000, window, [=] {
                check(!shown(card()) && !shown(tour()), QStringLiteral("Later (Esc) closes the card, and nothing follows"));
                onlySeen({}, QStringLiteral("Later leaves them unseen"));
                relaunch();
                when([=] { return shown(card()); }, 30, [=] {
                    const QStringList again = looked(QStringLiteral("the card at the next launch"));
                    QStringList missing;
                    for (const QString &id : *listed)
                        if (!again.contains(id))
                            missing << id;
                    check(missing.isEmpty(), QStringLiteral("it lists them again%1")
                                                 .arg(missing.isEmpty() ? QString() : ": not " + missing.join(", ")));
                    *listed = again;
                    grab(QStringLiteral("card-again"));
                    show();
                }, [=] {
                    check(false, QStringLiteral("the card again at the next launch"));
                    finish();
                });
            });
        });
    }, [=] {
        // What it needs, said: plugins on whose tours are not seen yet (in
        // $XDG_STATE_HOME/pinhole/toured.json, or plugins.json's "toured"
        // before it moves), and nothing over the window that has the keys.
        QStringList unseen;
        for (const QString &id : plugins->property("ids").toStringList())
            if (!toured(id))
                unseen << id;
        check(false, QStringLiteral("no tour nor the card showed by itself in 60 s (tours not seen: %1; keys: %2's)")
                         .arg(unseen.isEmpty() ? QStringLiteral("none, give it a state folder that has not seen them") : unseen.join(", "),
                              window->property("keyLayer").toString()));
        finish();
    });
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

void runSheetSelfTest(QQuickWindow *window, PhotoGrid *grid, const QString &folder) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    // In the plugin, by the items it shows, as Series' test finds its own.
    auto studio = [window] {
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("sheetStudio"))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(child))
                    return found;
            return nullptr;
        };
        return static_cast<QObject *>(find(window->contentItem()));
    };
    auto grab = [window, folder](const QString &name) { window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name)); };
    auto describe = [studio](const QString &name) {
        QObject *s = studio();
        const QVariantMap sheet = s->property("sheet").toMap();
        const QVariantMap recipe = s->property("recipe").toMap();
        return QStringLiteral("%1: %2, %3 photos, %4 %5x%6, gap %7, %8, dates %9")
            .arg(name, s->property("shown").toBool() ? "shown" : "gone")
            .arg(s->property("photos").toList().size())
            .arg(recipe.value("layout").toString()).arg(sheet.value("w").toInt()).arg(sheet.value("h").toInt())
            .arg(recipe.value("gap").toString(), recipe.value("background").toString(), recipe.value("dates").toBool() ? "on" : "off");
    };
    QTimer::singleShot(2500, window, [=] {
        window->setProperty("troubleSeen", true);
        if (!studio()) {
            say(QStringLiteral("FAIL no Contact sheet plugin"));
            QCoreApplication::exit(1);
            return;
        }
        // Seven photos from the middle of the screen (PINHOLE_SHEET_COUNT
        // for more), ticked as Space does.
        const int count = qEnvironmentVariableIsSet("PINHOLE_SHEET_COUNT") ? qEnvironmentVariableIntValue("PINHOLE_SHEET_COUNT") : 7;
        grid->setSelecting(true);
        const int first = std::max(0, std::min(grid->indexAt(grid->width() / 2, grid->height() / 2), grid->count() - count));
        for (int i = first; i < first + count; ++i) {
            grid->putCursorOn(i);
            grid->tickCursor();
        }
        say(QStringLiteral("%1 ticked").arg(grid->selection().size()));
        const QList<std::pair<QString, std::function<void()>>> steps = {
            { QStringLiteral("0-ticked"), [] {} },
            { QStringLiteral("1-open"), [window] { key(window, Qt::Key_K, Qt::NoModifier, "k"); } },
            { QStringLiteral("2-collage"), [window] { key(window, Qt::Key_2, Qt::NoModifier, "2"); } },
            { QStringLiteral("3-dates"), [window] { key(window, Qt::Key_D, Qt::NoModifier, "d"); } },
            { QStringLiteral("4-grid"), [window] { key(window, Qt::Key_1, Qt::NoModifier, "1"); } },
            { QStringLiteral("5-white"), [window] { key(window, Qt::Key_B, Qt::NoModifier, "b"); } },
            { QStringLiteral("6-wide"), [window] { key(window, Qt::Key_G, Qt::NoModifier, "g"); } },
        };
        // How long each key holds the window: until it is handled, until the
        // frame after it, and the longest the window then stood still while
        // the step settles (mediums arriving, after K).
        auto *ticks = new QTimer(window);
        auto sinceTick = std::make_shared<QElapsedTimer>();
        auto stall = std::make_shared<qint64>(0);
        ticks->setInterval(4);
        QObject::connect(ticks, &QTimer::timeout, window, [=] {
            *stall = std::max(*stall, sinceTick->restart() - 4);
        });
        sinceTick->start();
        ticks->start();
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [=](int n) {
            if (n == steps.size()) {
                // Enter: fetched, put together, copied; how far as it goes.
                key(window, Qt::Key_Return);
                auto clock = std::make_shared<QElapsedTimer>();
                clock->start();
                auto poll = std::make_shared<QTimer>();
                poll->setInterval(250);
                auto last = std::make_shared<QString>();
                QObject::connect(poll.get(), &QTimer::timeout, window, [=] {
                    QObject *s = studio();
                    const QString making = s->property("making").toString();
                    const QString now = QStringLiteral("%1 %2").arg(making).arg(s->property("fetched").toInt());
                    if (now != *last && making != "") {
                        *last = now;
                        say(QStringLiteral("making: %1, %2 fetched").arg(making).arg(s->property("fetched").toInt()));
                        if (making == QLatin1String("fetching") && s->property("fetched").toInt() == 0)
                            grab(QStringLiteral("7-fetching"));
                    }
                    const QVariantMap made = s->property("made").toMap();
                    const QString error = s->property("error").toString();
                    // Said copied once the clipboard has it.
                    if ((making != "" || (made.isEmpty() && error.isEmpty()) || made.value("copy") == QLatin1String("copying"))
                        && clock->elapsed() < 900000)
                        return;
                    poll->stop();
                    grab(QStringLiteral("8-made"));
                    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
                    if (!made.isEmpty())
                        say(QStringLiteral("made %1 (%2) in %3 ms, %4: clipboard image/png %5, image/jpeg %6, uri %7")
                                .arg(made.value("path").toString(), made.value("size").toString())
                                .arg(clock->elapsed()).arg(made.value("copy").toString())
                                .arg(mime && mime->hasFormat("image/png") ? "yes" : "no", mime && mime->hasFormat("image/jpeg") ? "yes" : "no",
                                     mime && mime->hasUrls() && mime->urls().value(0).toLocalFile() == made.value("path").toString() ? "yes" : "no"));
                    else
                        say(QStringLiteral("FAIL %1").arg(error.isEmpty() ? QStringLiteral("nothing in 900 s") : error));
                    // Made again, and Esc at once stops it, the studio
                    // still open; Esc again closes it.
                    key(window, Qt::Key_Return);
                    QTimer::singleShot(40, window, [=] {
                        key(window, Qt::Key_Escape);
                        QObject *s = studio();
                        say(QStringLiteral("Esc while making: %1, \"%2\", %3")
                                .arg(s->property("making").toString().isEmpty() ? "stopped" : "still making",
                                     s->property("note").toString(), s->property("shown").toBool() ? "open" : "closed"));
                        key(window, Qt::Key_Escape);
                        QTimer::singleShot(600, window, [=] {
                            say(describe(QStringLiteral("after Esc")) + QStringLiteral(", %1 of the making's still on its way")
                                    .arg(studio()->property("makePending").toList().size()));
                            QCoreApplication::exit(made.isEmpty() ? 1 : 0);
                        });
                    });
                });
                poll->start();
                return;
            }
            QElapsedTimer handled;
            handled.start();
            steps[n].second();
            const qint64 took = handled.elapsed();
            // The frame after it, drawn now (a window on a workspace not
            // shown draws none by itself).
            handled.restart();
            window->grabWindow();
            const qint64 frame = handled.elapsed();
            sinceTick->restart();
            *stall = 0;
            // Opening many: long enough for the first mediums to come.
            QTimer::singleShot(n == 1 ? 4000 : 1500, window, [=] {
                say(describe(steps[n].first) + QStringLiteral("; key %1 ms, its frame %2 ms, longest stall after %3 ms")
                    .arg(took).arg(frame).arg(*stall));
                grab(steps[n].first);
                (*step)(n + 1);
            });
        };
        (*step)(0);
    });
}

static bool pluginCheckPasses(const QString &plugin, const std::function<void(const QString &)> &say);

void runThemeSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &folder) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    // Never the desktop's own theme: Omarchy in a home of its own, and the
    // window watching that one.
    const QString home = QDir(QDir::homePath()).canonicalPath();
    const QString omarchyHome = qEnvironmentVariable("PINHOLE_OMARCHY_HOME");
    const QString themeDir = qEnvironmentVariable("PINHOLE_THEME_DIR");
    if (omarchyHome.isEmpty() || themeDir.isEmpty() || QDir(omarchyHome).canonicalPath() == home
        || QDir(themeDir).canonicalPath() == QDir(home + "/.local/state/omarchy/current").canonicalPath()) {
        say(QStringLiteral("refused: it would set the desktop's own theme; give PINHOLE_OMARCHY_HOME and "
                           "PINHOLE_THEME_DIR (its .local/state/omarchy/current), apart from $HOME"));
        QTimer::singleShot(0, window, [] { QCoreApplication::exit(1); });
        return;
    }
    auto grab = [window, folder](const QString &name) { window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name)); };
    // Omarchy's theme as omarchy-theme-set leaves it, where the window
    // watches it.
    auto current = [] {
        QFile file(qEnvironmentVariable("PINHOLE_THEME_DIR") + "/theme.name");
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
    };
    auto failed = std::make_shared<bool>(false);
    // Calls `then` once the theme is `wanted` (or is not `wanted`, with
    // `away`), a moment later for the window to retint; fails after 90 s.
    auto waitFor = [=](const std::function<bool(const QString &)> &done, const std::function<void()> &then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto poll = std::make_shared<QTimer>();
        poll->setInterval(200);
        QObject::connect(poll.get(), &QTimer::timeout, window, [=] {
            const QString name = current();
            if (!done(name) && clock->elapsed() < 90000)
                return;
            poll->stop();
            if (!done(name)) {
                say(QStringLiteral("the theme stayed %1").arg(name));
                *failed = true;
            }
            QTimer::singleShot(1500, window, [poll, then] { then(); });
        });
        poll->start();
    };
    // The photos by position, and the theme before, once the library is in.
    auto photos = std::make_shared<QList<int>>();
    auto before = std::make_shared<QString>();
    auto step = std::make_shared<std::function<void(int)>>();
    *step = [=, &viewer, &library](int n) {
        if (n == photos->size()) {
            // Then the script's making and pruning, in a home of its own
            // (plugins/theme/check/check): a theme of the user's stays.
            *failed = !pluginCheckPasses(QStringLiteral("theme"), say) || *failed;
            say(*failed ? QStringLiteral("FAILED") : QStringLiteral("ok"));
            QCoreApplication::exit(*failed ? 1 : 0);
            return;
        }
        const int index = (*photos)[n];
        viewer.openAt(index, QRectF(0, 0, 100, 100));
        QTimer::singleShot(2500, window, [=] {
            grab(QStringLiteral("%1-photo").arg(n));
            key(window, Qt::Key_T, Qt::NoModifier, QStringLiteral("t"));
            waitFor([before](const QString &name) { return name.startsWith(QLatin1String("pinhole-")) && name != *before; }, [=] {
                const QString made = current();
                say(QStringLiteral("%1: %2").arg(library.at(index).filename, made));
                grab(QStringLiteral("%1-themed").arg(n));
                key(window, Qt::Key_Z, Qt::ControlModifier);
                waitFor([before](const QString &name) { return name == *before; }, [=] {
                    say(QStringLiteral("undone: %1").arg(current()));
                    grab(QStringLiteral("%1-undone").arg(n));
                    (*step)(n + 1);
                });
            });
        });
    };
    // The library is read after the window opens; then the plugins load
    // and the window settles.
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto loaded = std::make_shared<QTimer>();
    loaded->setInterval(250);
    QObject::connect(loaded.get(), &QTimer::timeout, window, [=, &library] {
        if (library.count() == 0 && clock->elapsed() < 60000)
            return;
        loaded->stop();
        for (const QString &id : qEnvironmentVariable("PINHOLE_SELFTEST_PHOTOS").split(',', Qt::SkipEmptyParts))
            for (int i = 0; i < library.count(); ++i)
                if (library.at(i).id == id.toUtf8()) {
                    *photos << i;
                    break;
                }
        if (photos->isEmpty()) {
            say(QStringLiteral("none of PINHOLE_SELFTEST_PHOTOS is in the library"));
            QCoreApplication::exit(1);
            return;
        }
        *before = current();
        say(QStringLiteral("%1 photos; the theme is %2").arg(photos->size()).arg(*before));
        QTimer::singleShot(3000, window, [loaded, step] { (*step)(0); });
    });
    loaded->start();
}

// A plugin's own check of its script, plugins/<plugin>/check/check, which
// works in homes of its own: run to its end, what it says said. Found in
// PINHOLE_BUILTIN_PLUGINS, else in the checkout this window was built in.
static bool pluginCheckPasses(const QString &plugin, const std::function<void(const QString &)> &say) {
    const QString name = plugin + "/check/check";
    QString check;
    for (const QString &dir : {qEnvironmentVariable("PINHOLE_BUILTIN_PLUGINS"), QCoreApplication::applicationDirPath() + "/../../plugins"})
        if (check.isEmpty() && !dir.isEmpty() && QFileInfo::exists(dir + '/' + name))
            check = QDir::cleanPath(dir + '/' + name);
    if (check.isEmpty()) {
        say(QStringLiteral("no plugins/%1 here (WRONG)").arg(name));
        return false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QFileInfo(check).absoluteFilePath(), {});
    const bool ran = process.waitForFinished(180000);
    for (const QString &line : QString::fromUtf8(process.readAll()).split('\n', Qt::SkipEmptyParts))
        say(plugin + ": " + line);
    return ran && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void runBackgroundSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &folder) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    // Never the desktop's own background: Omarchy's shell answers whatever
    // HOME asks, so a home of its own, stand-ins for what reaches the shell,
    // and the backgrounds kept in that home.
    const QString home = QDir(QDir::homePath()).canonicalPath();
    const QString omarchyHome = QDir(qEnvironmentVariable("PINHOLE_OMARCHY_HOME")).canonicalPath();
    const QString stubs = qEnvironmentVariable("PINHOLE_OMARCHY_STUBS");
    const QString kept = QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/Pinhole/.backgrounds").absolutePath();
    if (qEnvironmentVariable("PINHOLE_OMARCHY_HOME").isEmpty() || omarchyHome.isEmpty() || omarchyHome == home
        || !QFileInfo(stubs + "/omarchy-theme-bg-set").isExecutable() || !QFileInfo(stubs + "/omarchy-shell").isExecutable()
        || !kept.startsWith(omarchyHome + '/')) {
        say(QStringLiteral("refused: it would set the desktop's own background; give PINHOLE_OMARCHY_HOME (apart from "
                           "$HOME, with XDG_PICTURES_DIR in it), and stand-ins for omarchy-theme-bg-set and "
                           "omarchy-shell in PINHOLE_OMARCHY_STUBS"));
        QTimer::singleShot(0, window, [] { QCoreApplication::exit(1); });
        return;
    }
    const QString state = omarchyHome + "/.local/state/omarchy";
    auto grab = [window, folder](const QString &name) { window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name)); };
    // What Omarchy's link points at now.
    auto current = [state] { return QFileInfo(state + "/current/background").canonicalFilePath(); };
    auto failed = std::make_shared<bool>(false);
    // Calls `then` once `done`, a moment later; fails after 60 s.
    auto waitFor = [=](const QString &what, const std::function<bool(const QString &)> &done, const std::function<void()> &then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto poll = std::make_shared<QTimer>();
        poll->setInterval(200);
        QObject::connect(poll.get(), &QTimer::timeout, window, [=] {
            const QString now = current();
            if (!done(now) && clock->elapsed() < 60000)
                return;
            poll->stop();
            say(QStringLiteral("%1: %2 (%3)").arg(what, now, done(now) ? "as it should" : "WRONG"));
            *failed = *failed || !done(now);
            grab(what);
            QTimer::singleShot(800, window, [poll, then] { then(); });
        });
        poll->start();
    };
    // Then the script's pruning, each case in a home of its own
    // (plugins/background/check/check): whatever the user put in reach
    // stays.
    auto finish = [=] {
        *failed = !pluginCheckPasses(QStringLiteral("background"), say) || *failed;
        say(*failed ? QStringLiteral("FAILED") : QStringLiteral("ok"));
        QCoreApplication::exit(*failed ? 1 : 0);
    };
    auto before = std::make_shared<QString>();
    auto set = std::make_shared<QString>();
    auto keptHere = [kept](const QString &path) { return path.startsWith(QDir(kept).canonicalPath() + '/'); };
    // Series' studio: its B on a video as if just made; the theme switched
    // to one with a background of the same name; Ctrl+Z.
    auto studio = [=] {
        const QString video = qEnvironmentVariable("PINHOLE_SELFTEST_VIDEO");
        std::function<QQuickItem *(QQuickItem *)> find = [&find](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("seriesStudio"))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(child))
                    return found;
            return nullptr;
        };
        QQuickItem *studioItem = find(window->contentItem());
        if (video.isEmpty() || !studioItem) {
            say(video.isEmpty() ? QStringLiteral("no PINHOLE_SELFTEST_VIDEO: the studio left out") : QStringLiteral("no studio"));
            *failed = *failed || !video.isEmpty();
            finish();
            return;
        }
        studioItem->setProperty("made", QVariantMap{{"path", video}, {"name", QFileInfo(video).fileName()}, {"size", ""}});
        QMetaObject::invokeMethod(studioItem, "makeBackground");
        waitFor(QStringLiteral("studio-set"), [=](const QString &now) { return keptHere(now) && now.endsWith(".mp4"); }, [=] {
            *set = current();
            // Another theme now, its own file of that name where the one
            // before was.
            QFile name(state + "/current/theme.name");
            name.open(QIODevice::ReadOnly);
            const QString was = QString::fromUtf8(name.readAll()).trimmed();
            name.close();
            name.open(QIODevice::WriteOnly | QIODevice::Truncate);
            name.write("test-other\n");
            name.close();
            QFile::remove(*before);
            QFile::copy(qEnvironmentVariable("PINHOLE_SELFTEST_OTHER"), *before);
            const QString own = omarchyHome + "/.config/omarchy/themes/" + was + "/backgrounds/" + QFileInfo(*before).fileName();
            say(QStringLiteral("the theme switched from %1; the one before is now %2").arg(was, own));
            key(window, Qt::Key_Z, Qt::ControlModifier);
            waitFor(QStringLiteral("studio-undone"), [own](const QString &now) { return now == QFileInfo(own).canonicalFilePath(); }, [=] {
                key(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
                waitFor(QStringLiteral("studio-redone"), [set](const QString &now) { return now == *set; }, finish);
            });
        });
    };
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto loaded = std::make_shared<QTimer>();
    loaded->setInterval(250);
    Viewer *shown = &viewer;
    QObject::connect(loaded.get(), &QTimer::timeout, window, [=, &library] {
        if (library.count() == 0 && clock->elapsed() < 60000)
            return;
        loaded->stop();
        int index = -1;
        const QString id = qEnvironmentVariable("PINHOLE_SELFTEST_PHOTOS").section(',', 0, 0);
        for (int i = 0; i < library.count(); ++i)
            if (library.at(i).id == id.toUtf8())
                index = i;
        if (index < 0) {
            say(QStringLiteral("the first of PINHOLE_SELFTEST_PHOTOS is not in the library"));
            QCoreApplication::exit(1);
            return;
        }
        *before = current();
        say(QStringLiteral("%1, the background %2").arg(library.at(index).filename, *before));
        QTimer::singleShot(3000, window, [=] {
            shown->openAt(index, QRectF(0, 0, 100, 100));
            QTimer::singleShot(2500, window, [=] {
                grab(QStringLiteral("photo"));
                key(window, Qt::Key_B, Qt::NoModifier, QStringLiteral("b"));
                waitFor(QStringLiteral("set"), [=](const QString &now) { return keptHere(now); }, [=] {
                    *set = current();
                    const QSize size = QImageReader(*set).size();
                    const bool small = size.isValid() && std::max(size.width(), size.height()) <= 3840;
                    say(QStringLiteral("kept %1x%2, %3 bytes (%4)").arg(size.width()).arg(size.height())
                        .arg(QFileInfo(*set).size()).arg(small ? "as it should" : "WRONG"));
                    *failed = *failed || !small;
                    key(window, Qt::Key_Z, Qt::ControlModifier);
                    waitFor(QStringLiteral("undone"), [before](const QString &now) { return now == *before; }, [=] {
                        key(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
                        waitFor(QStringLiteral("redone"), [set](const QString &now) { return now == *set; }, [=] {
                            key(window, Qt::Key_Z, Qt::ControlModifier);
                            waitFor(QStringLiteral("undone-again"), [before](const QString &now) { return now == *before; }, studio);
                        });
                    });
                });
            });
        });
    });
    loaded->start();
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
                    .arg(notice && notice->opacity() > 0 ? QStringLiteral(", saying \"%1\"").arg(notice->property("message").toString()) : QString()));
                *failed = *failed || now != wanted;
                grab(name);
                (*step)(n + 1);
            });
        };
        (*step)(0);
    });
    wait->start();
}

void runKeysSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, Keymap &keymap) {
    auto find = [window](const char *name) { return window->findChild<QObject *>(QString::fromLatin1(name)); };
    auto show = [find](const char *name, bool shown) {
        if (QObject *o = find(name))
            o->setProperty("shown", shown);
    };
    struct State {
        const char *what;
        std::function<void()> enter;
        const char *layer;
    };
    auto openMenu = [find](const char *bar) {
        // The first group with a menu in the bar: its ▾ as a press would.
        QObject *actions = find(bar);
        for (QObject *child : actions ? actions->findChildren<QObject *>() : QList<QObject *>())
            if (child->property("isGroup").toBool() && child->property("menuItems").toList().size() > 0) {
                QMetaObject::invokeMethod(child, "toggle", Q_ARG(QVariant, false));
                return;
            }
    };
    auto closeMenus = [find](const char *bar) {
        if (QObject *actions = find(bar))
            QMetaObject::invokeMethod(actions, "closeMenus");
    };
    // The plugins' buttons folded into More ▾ (MoreGroup.qml) under `bar`,
    // the window made narrower for them to fold where they fit; and its
    // menu opened as a press would.
    const QSize size = window->size();
    auto more = [find](const char *bar) -> QObject * {
        QObject *actions = find(bar);
        QQuickItem *button = nullptr;
        for (QQuickItem *b : actions ? actions->findChildren<QQuickItem *>(QStringLiteral("moreButton")) : QList<QQuickItem *>())
            if (b->isVisible())
                button = b;
        for (QQuickItem *item = button; item; item = item->parentItem())
            if (item->property("isGroup").toBool())
                return item;
        return nullptr;
    };
    auto openMore = [=](const char *bar) {
        auto open = [=] {
            QObject *group = more(bar);
            std::printf("selftest:      More ▾ in %s: %s\n", bar, group ? "shown, opened" : "NOT shown");
            if (group)
                QMetaObject::invokeMethod(group, "toggle", Q_ARG(QVariant, false));
        };
        if (more(bar)) {
            open();
        } else {
            window->resize(560, size.height());
            QTimer::singleShot(400, window, open);
        }
    };
    const int first = grid->indexAt(grid->width() / 2, grid->height() / 2);
    auto states = std::make_shared<QList<State>>(QList<State>{
        {"the grid", [] {}, "grid"},
        // ↑ ↓ ⏎ and Esc the menu's, not the grid's.
        {"the grid's More menu", [=] { openMore("gridActions"); }, "menu"},
        {"the grid's More menu closed by Esc", [=] { key(window, Qt::Key_Escape); }, "grid"},
        {"select mode", [=] { window->resize(size); grid->setSelecting(true); }, "grid"},
        {"select mode's menu", [=] { grid->selectAll(); openMenu("selectBar"); }, "menu"},
        {"the grid again", [=] { closeMenus("selectBar"); grid->setSelecting(false); }, "grid"},
        {"the viewer", [=, &viewer] { viewer.openAt(first, grid->rectOf(first)); }, "viewer"},
        {"the viewer's menu", [=] { openMenu("viewerActions"); }, "menu"},
        {"the viewer's More menu", [=] { closeMenus("viewerActions"); openMore("viewerActions"); }, "menu"},
        {"the viewer's More menu closed by Esc", [=] { key(window, Qt::Key_Escape); }, "viewer"},
        {"the help over the viewer", [=] { window->resize(size); show("help", true); }, "help"},
        {"the viewer closed", [=] {
             show("help", false);
             if (QObject *photoViewer = find("photoViewer"))
                 QMetaObject::invokeMethod(photoViewer, "closeViewer");
         }, "grid"},
        {"the side panel", [=] { show("sidePanel", true); }, "side"},
        {"the help over the side panel", [=] { show("help", true); }, "help"},
        {"sign-in over the side panel", [=] { show("help", false); show("signIn", true); }, "signIn"},
        {"adding a source", [=] { show("signIn", false); show("sidePanel", false); show("addSource", true); }, "addSource"},
        {"the grid at the end", [=] { show("addSource", false); }, "grid"},
    });
    auto failures = std::make_shared<int>(0);
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=, &keymap](int k) {
        if (k >= states->size()) {
            const QStringList missing = keymap.uncovered();
            for (const QString &m : missing)
                std::printf("selftest: FAIL no way by %s\n", qPrintable(m));
            *failures += int(missing.size());
            std::printf("selftest: %d commands; done, %d failed\n", int(keymap.actions().size()), *failures);
            std::fflush(stdout);
            QCoreApplication::exit(*failures ? 1 : 0);
            return;
        }
        (*states)[k].enter();
        // Long enough for the viewer to close.
        QTimer::singleShot(900, window, [=, &keymap] {
            const State &state = (*states)[k];
            const QStringList clashes = keymap.clashes();
            const bool layerRight = keymap.layer() == QLatin1String(state.layer);
            // Never a field hidden with the keys (sign-in's, closed).
            QQuickItem *focus = window->activeFocusItem();
            const bool focusShown = !focus || (focus->isVisible() && focus->isEnabled());
            if (!focusShown)
                std::printf("selftest:      the keys are with a hidden %s\n", focus->metaObject()->className());
            const bool ok = clashes.isEmpty() && layerRight && focusShown;
            *failures += ok ? 0 : 1;
            std::printf("selftest: %-4s %s: the keys are %s's%s\n", ok ? "ok" : "FAIL", state.what, qPrintable(keymap.layer()),
                        layerRight ? "" : qPrintable(QStringLiteral(" (not %1's)").arg(QLatin1String(state.layer))));
            for (const QString &clash : clashes)
                std::printf("selftest:      two at once: %s\n", qPrintable(clash));
            std::fflush(stdout);
            (*run)(k + 1);
        });
    };
    QTimer::singleShot(1500, window, [run] { (*run)(0); });
}

void runInputsSelfTest(QQuickWindow *window, PhotoGrid *grid, const QString &dir) {
    auto find = [window](const char *name) { return window->findChild<QObject *>(QString::fromLatin1(name)); };
    auto grab = [window, dir](const char *name) { window->grabWindow().save(dir + '/' + QLatin1String(name) + ".png"); };
    auto steps = std::make_shared<QList<std::function<void()>>>(QList<std::function<void()>>{
        // At the oldest photos, so the cursor is at the top left, in any
        // grab of a window wider than the screen.
        [=] { key(window, Qt::Key_Home); },
        [=] { key(window, Qt::Key_Down); },
        [=] { key(window, Qt::Key_Right); },
        // A grab on screen comes back with a frame from a moment later: each
        // has a step of its own, nothing changing just after it.
        [=] { grab("1-cursor"); },
        [=] { key(window, Qt::Key_PageDown); },
        [=] { grab("2-paged"); },
        [=] { key(window, Qt::Key_Home); },
        [=] { grid->setSelecting(true); key(window, Qt::Key_Space); },
        [=] { key(window, Qt::Key_Right, Qt::ShiftModifier); },
        [=] { key(window, Qt::Key_Down, Qt::ShiftModifier); },
        [=] { grab("3-select"); },
        [=] {
            grid->setSelecting(false);
            if (QObject *notice = find("historyNotice"))
                QMetaObject::invokeMethod(notice, "show", Q_ARG(QVariant, QStringLiteral("Favourite")), Q_ARG(QVariant, QStringLiteral("undo")));
        },
        [=] { grab("4-undo"); },
        [=] { if (QObject *side = find("sidePanel")) side->setProperty("shown", true); },
        [=] { for (int i = 0; i < 6; ++i) key(window, Qt::Key_Down); },
        [=] { grab("5-side"); },
        [=] { QCoreApplication::quit(); },
    });
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=](int k) {
        if (k >= steps->size())
            return;
        (*steps)[k]();
        QTimer::singleShot(700, window, [=] { (*run)(k + 1); });
    };
    QTimer::singleShot(2500, window, [run] { (*run)(0); });
}

void runOtherSeriesSelfTest(QQuickWindow *window, const Library &library, LibraryReloader &reloader, const QString &dir) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    // As the window shows it: a plugin's items are the window's items'
    // children, not its objects'.
    auto plugin = [window]() -> QObject * {
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("seriesEditor"))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(child))
                    return found;
            return nullptr;
        };
        QQuickItem *editor = find(window->contentItem());
        return editor ? editor->property("plugin").value<QObject *>() : nullptr;
    };
    QObject *history = qmlEngine(window)->rootContext()->contextProperty("history").value<QObject *>();
    // A QML var: a QJSValue, or already a QVariant.
    auto plain = [](const QVariant &v) { return v.metaType() == QMetaType::fromType<QJSValue>() ? v.value<QJSValue>().toVariant() : v; };
    auto manual = [=]() {
        QList<QStringList> series;
        for (const QVariant &ids : plain(plugin()->property("edits")).toMap().value("manual").toList())
            series.append(ids.toStringList());
        return series;
    };
    // As written in its state, once all of it is.
    auto keptFor = [=](const QString &key) {
        if (auto *plugins = qobject_cast<Plugins *>(qmlEngine(window)->rootContext()->contextProperty("plugins").value<QObject *>()))
            plugins->flush();
        QFile file(Plugins::stateRoot() + "/pinhole.series/state.json");
        if (!file.open(QIODevice::ReadOnly))
            return QList<QStringList>();
        QList<QStringList> series;
        const QJsonObject edits = QJsonDocument::fromJson(file.readAll()).object()[key].toObject();
        for (const QVariant &ids : edits.toVariantMap().value("manual").toList())
            series.append(ids.toStringList());
        return series;
    };
    auto failed = std::make_shared<bool>(false);
    auto check = [=](const QString &what, bool ok) {
        say(QStringLiteral("%1: %2").arg(what, ok ? "as it should" : "WRONG"));
        *failed = *failed || !ok;
    };
    // Calls `then` once `ready` holds, polled; gives up after 60 s.
    auto when = [=](const QString &what, std::function<bool()> ready, std::function<void()> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto poll = new QTimer(window);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, window, [=] {
            if (ready()) {
                poll->deleteLater();
                then();
            } else if (clock->elapsed() > 60000) {
                poll->deleteLater();
                QObject *p = plugin();
                say(QStringLiteral("no %1 in 60 s (Series %2, its edits' key \"%3\"); FAILED")
                        .arg(what, p ? "loaded" : "not loaded", p ? p->property("editsKey").toString() : QString()));
                QCoreApplication::exit(1);
            }
        });
        poll->start();
    };
    const QString first = library.dir(), firstSource = library.source();
    auto shown = [=, &library](const QString &source) {
        return [=, &library] {
            return library.source() == source && plugin() && plugin()->property("editsKey").toString() == "edits:" + source;
        };
    };
    when(QStringLiteral("library with Series"), [=, &library] { return library.count() >= 3 && shown(firstSource)(); }, [=, &library, &reloader] {
        const QStringList ids{library.at(0).id, library.at(1).id, library.at(2).id};
        const QVariant indexes = QVariantList{0, 1, 2}, name = QStringLiteral("grouping");
        QMetaObject::invokeMethod(plugin(), "group", Q_ARG(QVariant, indexes), Q_ARG(QVariant, name));
        check(QStringLiteral("grouped in %1").arg(firstSource), manual() == QList<QStringList>{ids});
        check(QStringLiteral("to undo"), history->property("canUndo").toBool());
        // Shown from elsewhere: no Sources::switched in this window.
        Library other;
        QString error;
        if (!other.open(dir, &error) || other.source() == firstSource || other.count() < 3 || other.at(0).id != library.at(0).id) {
            say(QStringLiteral("%1 is not another source's library of the same photos; FAILED").arg(dir));
            QCoreApplication::exit(1);
            return;
        }
        const QString otherSource = other.source();
        // The service says it shows the other, before the window has read
        // its library: nothing to undo already.
        QObject *service = qmlEngine(window)->rootContext()->contextProperty("service").value<QObject *>();
        const QJsonObject said{{"event", "source"}, {"id", otherSource}};
        QMetaObject::invokeMethod(service, "announced", Q_ARG(QString, QStringLiteral("source")), Q_ARG(QJsonObject, said));
        check(QStringLiteral("nothing to undo once %1 is shown, before it is read").arg(otherSource),
              library.source() == firstSource && !history->property("canUndo").toBool());
        reloader.moveTo(dir);
        when(QStringLiteral("other library"), shown(otherSource), [=, &reloader] {
            check(QStringLiteral("nothing grouped in %1").arg(otherSource), manual().isEmpty());
            check(QStringLiteral("nothing to undo there"), !history->property("canUndo").toBool());
            QMetaObject::invokeMethod(history, "undo");
            QTimer::singleShot(500, window, [=, &reloader] {
                check(QStringLiteral("still nothing grouped after Ctrl+Z"), manual().isEmpty());
                check(QStringLiteral("%1 keeps its group").arg(firstSource), keptFor("edits:" + firstSource) == QList<QStringList>{ids});
                check(QStringLiteral("%1 has no edits kept").arg(otherSource), keptFor("edits:" + otherSource).isEmpty());
                reloader.moveTo(first);
                when(QStringLiteral("first library again"), shown(firstSource), [=] {
                    QTimer::singleShot(500, window, [=] {
                        check(QStringLiteral("grouped again in %1").arg(firstSource), manual() == QList<QStringList>{ids});
                        const QVariantList series = plain(plugin()->property("series")).toList();
                        check(QStringLiteral("shown as a series"), std::any_of(series.begin(), series.end(), [](const QVariant &s) {
                            QList<int> at;
                            for (const QVariant &i : s.toList())
                                at.append(i.toInt());
                            return at == QList<int>{0, 1, 2};
                        }));
                        say(*failed ? QStringLiteral("FAILED") : QStringLiteral("ok"));
                        QCoreApplication::exit(*failed ? 1 : 0);
                    });
                });
            });
        });
    });
}
