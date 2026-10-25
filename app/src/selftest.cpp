#include "selftest.h"

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
#include <QQuickItem>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QVideoFrame>
#include <QVideoSink>
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
        {"HEIC photo", "EXISTS (SELECT 1 FROM resources r WHERE r.asset_id = a.id AND r.kind = 'Original' AND r.file_type = 'public.heic') AND NOT a.is_live"},
        {"JPEG photo", "EXISTS (SELECT 1 FROM resources r WHERE r.asset_id = a.id AND r.kind = 'Original' AND r.file_type = 'public.jpeg')"},
        {"video", "a.is_video AND a.duration < 60"},
        {"long video", "a.is_video AND a.duration > 300"},
        {"Live Photo", "a.is_live", true},
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
            if (viewer.stage() == QLatin1String("original")) {
                if (c.live)
                    viewer.playLive();
                else
                    finish();
            }
        }));
        connections->append(QObject::connect(&viewer, &Viewer::videoChanged, &viewer, [&viewer, c, report, clock, finish] {
            if (viewer.videoSource().isEmpty())
                return;
            report(c.name, viewer.videoSource().isLocalFile() ? "file" : "stream", QString());
            playFirstFrame(viewer.videoSource(), c.name, clock, finish);
        }));
        connections->append(QObject::connect(&viewer, &Viewer::liveChanged, &viewer, [&viewer, c, report, finish] {
            if (viewer.liveSource().isEmpty())
                return;
            report(c.name, "motion", QString());
            finish();
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
    };
    QTimer::singleShot(500, [next] { (*next)(0); });
}

void runOutgoingSelfTest(const Library &library, Outgoing &outgoing, const QString &folder) {
    const QList<int> picks = {
        findIndex(library, "EXISTS (SELECT 1 FROM resources r WHERE r.asset_id = a.id AND r.kind = 'Original' AND r.file_type = 'public.heic') AND NOT a.is_live"),
        findIndex(library, "EXISTS (SELECT 1 FROM resources r WHERE r.asset_id = a.id AND r.kind = 'Original' AND r.file_type = 'public.jpeg')"),
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
                    std::printf("selftest:   clipboard: %lld file(s), image %s, gnome-copied-files %s\n",
                                mime ? qlonglong(mime->urls().size()) : 0LL,
                                mime && mime->hasImage() ? "yes" : "no",
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

void mouse(QQuickWindow *window, QEvent::Type type, QPointF at, Qt::MouseButton button,
           Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, at, window->mapToGlobal(at), button, held, mods);
    QGuiApplication::sendEvent(window, &event);
}

void click(QQuickWindow *window, QPointF at, Qt::MouseButton button = Qt::LeftButton,
           Qt::KeyboardModifiers mods = Qt::NoModifier) {
    mouse(window, QEvent::MouseButtonPress, at, button, mods);
    mouse(window, QEvent::MouseButtonRelease, at, button, mods);
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
    static quint64 clock = 100000;
    clock += 1000;
    wheel(window, at, {}, {}, Qt::ScrollBegin, clock);
    for (int i = 0; i < steps; ++i)
        wheel(window, at, by, by * 8, Qt::ScrollUpdate, clock += 8);
    wheel(window, at, {}, {}, Qt::ScrollEnd, clock += 8);
}

} // namespace

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
    const QPointF middle(window->width() / 2.0, window->height() / 2.0);
    const QPointF right(window->width() - 40.0, window->height() / 2.0);
    const QPointF left(40.0, window->height() / 2.0);
    auto menu = [window] { return window->findChild<QObject *>(QStringLiteral("photoMenu")); };
    auto steps = std::make_shared<QList<Step>>(QList<Step>{
        {"right-click opens the menu for that photo, selecting nothing",
         [=] {
             *start = grid->indexAt(middle.x(), middle.y());
             click(window, middle, Qt::RightButton);
         },
         [=] {
             return grid->selectionCount() == 0 && menu() && menu()->property("opened").toBool() &&
                    menu()->property("target").toInt() == *start;
         }},
        {"the menu closes", [=] { QMetaObject::invokeMethod(menu(), "close"); },
         [=] { return !menu()->property("visible").toBool(); }},
        {"a click opens the photo, selecting nothing", [=] { click(window, middle); },
         [=, &viewer] { return viewer.isOpen() && viewer.index() == *start && grid->selectionCount() == 0; }, 900},
        {"clicking the right side shows the next", [=] { click(window, right); },
         [&viewer, start] { return viewer.index() == *start + 1; }},
        {"clicking the left side shows the previous", [=] { click(window, left); },
         [&viewer, start] { return viewer.index() == *start; }},
        {"a wheel notch down shows the next", [=] { wheel(window, middle, {}, {0, -120}); },
         [&viewer, start] { return viewer.index() == *start + 1; }},
        {"a long swipe to the left shows the next, once", [=] { swipe(window, middle, {-12, 2}, 30); },
         [&viewer, start] { return viewer.index() == *start + 2; }},
        {"two short swipes to the right show two before", [=] {
             swipe(window, middle, {8, -1}, 8);
             swipe(window, middle, {8, 1}, 8);
         },
         [&viewer, start] { return viewer.index() == *start; }},
        {"a swipe left again shows the next", [=] { swipe(window, middle, {-10, 0}, 10); },
         [&viewer, start] { return viewer.index() == *start + 1; }},
        {"scrolling up and down with two fingers does nothing", [=] { swipe(window, middle, {2, -15}, 30); },
         [&viewer, start] { return viewer.index() == *start + 1; }},
        {"a slightly slanted swipe still counts", [=] { swipe(window, middle, {-10, 5}, 10); },
         [&viewer, start] { return viewer.index() == *start + 2; }},
        {"mid-swipe the photo follows the fingers",
         [=] {
             wheel(window, middle, {}, {}, Qt::ScrollBegin, 900000);
             for (int i = 0; i < 5; ++i)
                 wheel(window, middle, {-10, 0}, {-80, 0}, Qt::ScrollUpdate, 900000 + 8 * (i + 1));
         },
         [&viewer] { return viewer.property("offset").toDouble() < -80; }, 100},
        {"stopping before lifting springs back to the same photo",
         [=] { wheel(window, middle, {}, {}, Qt::ScrollEnd, 900400); },
         [&viewer, start] { return viewer.index() == *start + 2 && std::abs(viewer.property("offset").toDouble()) < 1; }, 900},
        {"a pinch zooms in", [=] { pinch(window, middle, 2.0); },
         [&viewer] { return std::abs(viewer.property("zoom").toDouble() - 2.0) < 0.05; }},
        {"zoomed, a swipe pans instead of paging", [=] { swipe(window, middle, {-6, -6}, 20); },
         [&viewer, start] {
             // Across or down, as far as the zoomed photo has room to move.
             const double moved = std::abs(viewer.property("panX").toDouble()) + std::abs(viewer.property("panY").toDouble());
             return viewer.index() == *start + 2 && std::abs(viewer.property("offset").toDouble()) < 1 && moved > 50;
         }},
        {"a flick on a zoomed photo coasts on after the fingers lift",
         [=, &viewer] {
             swipe(window, middle, {0, 14}, 8);
             *mark = viewer.property("panY").toDouble();
         },
         [=, &viewer] { return viewer.property("panY").toDouble() > *mark + 30; }, 250},
        {"a double-click zoomed goes back to fitted", [=] { doubleClick(window, middle); },
         [&viewer] { return viewer.isOpen() && viewer.property("zoom").toDouble() < 1.02; }, 900},
        {"clicking a thumbnail in the carousel shows that photo",
         [=] { click(window, QPointF(window->width() / 2.0 + 3 * 38, window->height() - 30.0)); },
         [=, &viewer] {
             if (viewer.index() != *start + 5)
                 std::printf("selftest:      (shows %d, wanted %d)\n", viewer.index() - *start, 5);
             return viewer.index() == *start + 5;
         }},
        {"pinching closed goes back to the library", [=] { pinch(window, middle, 0.5); },
         [&viewer] { return !viewer.isOpen(); }, 900},
        {"a click opens it again", [=] { click(window, grid->rectOf(*start + 5).center()); },
         [&viewer] { return viewer.isOpen(); }, 900},
        {"right-click in the viewer opens the menu", [=] { click(window, middle, Qt::RightButton); },
         [=] { return menu()->property("opened").toBool(); }},
        {"the menu closes again", [=] { QMetaObject::invokeMethod(menu(), "close"); },
         [=] { return !menu()->property("visible").toBool(); }},
        {"the mouse's back button goes back to the library", [=] { click(window, middle, Qt::BackButton); },
         [&viewer] { return !viewer.isOpen(); }},
        {"a double-click opens it and leaves it open", [=] { doubleClick(window, grid->rectOf(*start).center()); },
         [&viewer] { return viewer.isOpen(); }, 900},
        {"double-click in the viewer goes back", [=] { doubleClick(window, middle); },
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
         [=] { click(window, QPointF(window->width() * 0.75, window->height() - 60.0 - 8.0)); },
         [=] {
             *mark = at();
             return std::abs(*mark / length() - 0.75) < 0.03;
         }, 1500},
        {"Shift+Left goes back ten seconds",
         [=] { QMetaObject::invokeMethod(video(), "skip", Q_ARG(QVariant, -10)); },
         [=] { return at() < *mark - 5000 && at() > *mark - 12000; }, 1500},
        {"the video closes", [=] { click(window, middle, Qt::BackButton); },
         [&viewer] { return !viewer.isOpen(); }},
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
                        ok ? "" : qPrintable(QStringLiteral(" (viewer %1 at start%2%3, offset %4)")
                                                 .arg(viewerPtr->isOpen() ? "open" : "closed")
                                                 .arg(viewerPtr->index() - *start >= 0 ? "+" : "")
                                                 .arg(viewerPtr->index() - *start)
                                                 .arg(viewerPtr->property("offset").toDouble(), 0, 'f', 0)));
            std::fflush(stdout);
            (*run)(k + 1);
        });
    };
    QTimer::singleShot(1200, window, [run] { (*run)(0); });
}
