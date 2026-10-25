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
#include <QProcess>
#include <QElapsedTimer>
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

// Opens each kind of asset in the viewer, as a person would, and reports
// how long each stage took to arrive and whether the files decode.
void runViewerSelfTest(const Library &library, Viewer &viewer) {
    const QList<Case> cases = {
        {"HEIC photo", "EXISTS (SELECT 1 FROM resources r WHERE r.asset_id = a.id AND r.kind = 'Original' AND r.file_type = 'public.heic') AND NOT a.is_live"},
        {"JPEG photo", "EXISTS (SELECT 1 FROM resources r WHERE r.asset_id = a.id AND r.kind = 'Original' AND r.file_type = 'public.jpeg')"},
        {"video", "a.is_video AND a.duration < 60"},
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
        connections->append(QObject::connect(&viewer, &Viewer::videoChanged, &viewer, [&viewer, c, report, finish] {
            if (viewer.videoSource().isEmpty())
                return;
            report(c.name, "video", QString());
            finish();
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
        outgoing.prepare(picks, false, folder + "/originals", [&outgoing, picks, folder, describe, &library](const QStringList &files, int failed) {
            describe(files);
            std::printf("selftest:   %d failed\nselftest: compatible\n", failed);
            outgoing.prepare(picks, true, folder + "/compatible", [&outgoing, picks, describe, &library](const QStringList &files, int failed) {
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
