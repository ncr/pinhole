#include <QBuffer>
#include <QClipboard>
#include <QColorSpace>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <array>
#include <csignal>
#include <pwd.h>
#include <unistd.h>
#include <lcms2.h>
#include <libheif/heif.h>
#include <turbojpeg.h>
#include <ultrahdr_api.h>

#include <qpa/qwindowsysteminterface.h>

#include "colour.h"
#include "display.h"
#include "stage.h"
#include "color-management-v1-client-protocol.h"
#include "gainmap.h"
#include "sdrvideo.h"
#include "encoder.h"
#include "favourites.h"
#include "history.h"
#include "keymap.h"
#include "sources.h"
#include "library.h"
#include "timeline.h"
#include "libraryreloader.h"
#include "markchoices.h"
#include "marks.h"
#include "outgoing.h"
#include "photogrid.h"
#include "pluginguide.h"
#include "framestats.h"
#include "plugins.h"
#include "photoprovider.h"
#include "thumbnailloader.h"
#include "scrollphysics.h"
#include "selftest.h"
#include "serviceclient.h"
#include "storage.h"
#include "updater.h"
#include "theme.h"
#include "translator.h"
#include "viewer.h"
#include "viewermotion.h"
#include "zoomer.h"

class PinholeTests : public QObject {
    Q_OBJECT

private:
    Q_SLOT void bundledPluginDocumentsAreReadableAndTheirLinksResolve() {
        PluginGuide guide;
        QCOMPARE(guide.api(), Plugins::api);
        QCOMPARE(PluginGuide::documents(), (QStringList{"plugin-quickstart", "plugins", "source-providers", "plugin-agents"}));
        QCOMPARE(guide.property("documentIds").toStringList(), PluginGuide::documents());
        for (const QString &internal : {QStringLiteral("hdr"), QStringLiteral("service"), QStringLiteral("sources")}) {
            QVERIFY(PluginGuide::document(internal).isEmpty());
            QVERIFY(guide.destination(internal + ".md", "plugins").isEmpty());
            QVERIFY(!QFile::exists(":/docs/" + internal + ".md"));
        }
        for (const QString &name : PluginGuide::documents()) {
            QVERIFY2(!PluginGuide::document(name).isEmpty(), qPrintable(name));
            // Every relative Markdown link shown in the offline reader must
            // resolve there, rather than silently pointing outside its scope.
            bool fenced = false;
            const QRegularExpression links(QStringLiteral("\\[[^\\]]+\\]\\(([^)]+)\\)"));
            for (const QString &line : PluginGuide::document(name).split('\n')) {
                if (line.trimmed().startsWith("```") || line.trimmed().startsWith("~~~")) {
                    fenced = !fenced;
                    continue;
                }
                if (fenced) continue;
                for (auto matches = links.globalMatch(line); matches.hasNext();) {
                    const QString link = matches.next().captured(1);
                    if (QUrl(link).isRelative())
                        QVERIFY2(!guide.destination(link, name).isEmpty(), qPrintable(name + ": " + link));
                }
            }
            const auto parts = guide.sections(name);
            QVERIFY(!parts.isEmpty());
            QSet<QString> ids;
            for (const QVariant &part : parts) {
                const auto section = part.toMap();
                const auto id = section.value("id").toString();
                QVERIFY(!ids.contains(id));
                ids.insert(id);
                QVERIFY(!guide.markdown(name, id).isEmpty());
            }
        }
        QCOMPARE(guide.destination("plugins.md#the-kit", "plugin-quickstart").value("document").toString(), QString("plugins"));
        QCOMPARE(guide.destination("plugins.md#pinhole", "plugin-quickstart").value("document").toString(), QString("plugins"));
        QCOMPARE(guide.destination("#promises", "plugins").value("document").toString(), QString("plugins"));
        QVERIFY(guide.destination("../plugins.md", "plugins").isEmpty());
        QVERIFY(guide.destination("file:///etc/passwd", "plugins").isEmpty());
        QVERIFY(guide.destination("https://example.org", "plugins").isEmpty());
        QVERIFY(guide.destination("plugins.md#no-such-section", "plugins").isEmpty());
        QVERIFY(guide.markdown("not-a-document").isEmpty());
        QVERIFY(guide.markdown("plugin-agents").contains("pinhole plugin docs --json"));
    }

    Q_SLOT void frameDiagnosticsStayBoundedWithTheirOverlayClosed() {
        FrameStats stats;
        for (int i = 0; i < 70000; ++i) {
            stats.syncBegan(); stats.renderBegan(); stats.renderEnded(); stats.frameSwapped();
        }
        QCOMPARE(stats.summary().frames, 1 << 16);
        QVERIFY(!stats.aroundWorst(1, 1).isEmpty());
        stats.reset();
        QCOMPARE(stats.summary().frames, 0);
    }

    Q_SLOT void thePluginKitSideWheelTraversesItsWholeRangeAndReversesAtTheEnds() {
        Zoomer zoom;
        zoom.setLevels({24, 48, 96, 192, 384});
        const auto turn = [&](int angle, int packets) {
            for (int i = 0; i < packets; ++i) {
                QWheelEvent event({30, 30}, {30, 30}, {}, {angle, 0}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                zoom.takeWheel(&event);
                zoom.advance(.01);
            }
            for (int i = 0; i < 100; ++i) zoom.advance(.01);
        };
        zoom.setLevel(0);
        turn(30, 80);
        QCOMPARE(zoom.level(), 4);
        QVERIFY(std::abs(zoom.shown() - 384) < 1);
        // Still the same turn: no rest and lots of redundant input at the
        // endpoint, then a reversal must move immediately.
        turn(120, 80);
        turn(-30, 80);
        QCOMPARE(zoom.level(), 0);
        QVERIFY(std::abs(zoom.shown() - 24) < 1);
    }

    // A flick: fingers moving `perEvent` px every 8 ms for `events` reports,
    // then lifted at once.
    static ScrollPhysics flicked(double perEvent, int events) {
        ScrollPhysics p;
        p.setBounds(0, 1e9, 1000);
        p.jumpTo(500000);
        double t = 0;
        p.begin(t);
        for (int i = 0; i < events; ++i)
            p.move(t += 8, perEvent);
        p.end(t + 8);
        return p;
    }

    static void settle(ScrollPhysics &p) {
        for (int i = 0; i < 2000 && p.animating(); ++i)
            p.step(1.0 / 120);
    }

    // A library in `dir` holding `rows` (id, date) and nothing else of note;
    // all Live Photos, or none.
    static void makeLibrary(const QString &dir, const QList<std::pair<QByteArray, qint64>> &rows, bool live = false,
                            const QString &source = {}) {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "make");
            db.setDatabaseName(dir + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE assets (id TEXT, thumb_path TEXT, avg_color INTEGER, thumb_w INTEGER, thumb_h INTEGER,"
                           " duration REAL, asset_date INTEGER, is_video INTEGER, is_live INTEGER, favorite INTEGER, filename TEXT)"));
            for (const auto &[id, date] : rows) {
                q.prepare("INSERT INTO assets VALUES (?, '', 0, 1, 1, 0, ?, 0, ?, 0, ?)");
                q.addBindValue(QString::fromUtf8(id));
                q.addBindValue(date);
                q.addBindValue(live ? 1 : 0);
                q.addBindValue(QString::fromUtf8(id) + ".HEIC");
                QVERIFY(q.exec());
            }
            if (!source.isEmpty()) {
                QVERIFY(q.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)"));
                q.prepare("INSERT INTO meta VALUES ('source', ?)");
                q.addBindValue(source);
                QVERIFY(q.exec());
            }
            db.close();
        }
        QSqlDatabase::removeDatabase("make");
    }

    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
    static QString read(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }

    // A stand-in for pinholed: the lines asked, and answers written by hand.
    struct FakeService {
        QLocalServer server;
        QLocalSocket *peer = nullptr;
        QList<QJsonObject> asked;
        void read() {
            while (peer && peer->canReadLine())
                asked.append(QJsonDocument::fromJson(peer->readLine()).object());
        }
        // The `live` fetches asked for so far.
        QList<QJsonObject> live() {
            read();
            QList<QJsonObject> found;
            for (const QJsonObject &line : std::as_const(asked))
                if (line.value("params").toObject().value("kind").toString() == QLatin1String("live"))
                    found.append(line);
            return found;
        }
        void send(const QJsonObject &line) {
            peer->write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
            peer->flush();
        }
        // The service's first line, as a service of this window's protocol.
        void greet() { send({{"event", "hello"}, {"protocol", ServiceClient::kProtocol}, {"version", "1.0.0"}}); }
    };

    // Plugins as the window has them, each of `manifests` (its id, and
    // what its manifest.json says besides) a folder with a Main.qml, and
    // `pinhole` in the engine's JavaScript the first one's host. Settings
    // go to the test's own config folder, started empty. With `withService`,
    // a stand-in for pinholed (`fake`) answers.
    struct PluginBench {
        QTemporaryDir dir;
        FakeService fake;
        Library library;
        std::unique_ptr<ServiceClient> service;
        std::unique_ptr<Viewer> viewer;
        std::unique_ptr<Translator> translator;
        History history;
        QQmlEngine engine;
        std::unique_ptr<Plugins> plugins;
        PluginHost *host = nullptr;

        explicit PluginBench(const QList<std::pair<QString, QJsonObject>> &manifests,
                             const QList<std::pair<QByteArray, qint64>> &photos = {{"a", 1000}, {"b", 2000}, {"c", 3000}},
                             bool withService = false) {
            QStandardPaths::setTestModeEnabled(true);
            QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json");
            forgetKept();
            makeLibrary(dir.path(), photos, false, "folder:/x");
            QString error;
            library.open(dir.path(), &error);
            qputenv("PINHOLE_SOCKET", (dir.path() + "/none.sock").toUtf8());
            if (withService)
                QVERIFY(fake.server.listen(dir.path() + "/none.sock"));
            service = std::make_unique<ServiceClient>(dir.path());
            if (withService) {
                QTRY_VERIFY(fake.server.hasPendingConnections());
                fake.peer = fake.server.nextPendingConnection();
                fake.greet();
            }
            viewer = std::make_unique<Viewer>(&library, service.get());
            translator = std::make_unique<Translator>(dir.path() + "/language", PINHOLE_I18N);
            QDir(dir.path()).mkdir("plugins");
            for (const auto &[id, extra] : manifests) {
                QJsonObject manifest{{"schemaVersion", 1}, {"id", id}, {"name", id}, {"entryPoints", QJsonObject{{"main", "Main.qml"}}}};
                for (auto it = extra.begin(); it != extra.end(); ++it)
                    manifest.insert(it.key(), it.value());
                QDir(dir.path() + "/plugins").mkdir(id);
                write(dir.path() + "/plugins/" + id + "/manifest.json", QJsonDocument(manifest).toJson());
                write(dir.path() + "/plugins/" + id + "/Main.qml", "import QtQuick\nItem {}\n");
            }
            qputenv("PINHOLE_BUILTIN_PLUGINS", (dir.path() + "/plugins").toUtf8());
            plugins = std::make_unique<Plugins>(&library, service.get(), viewer.get(), translator.get(), &history);
            plugins->setEngine(&engine);
            plugins->libraryOpened();
            if (!manifests.isEmpty()) {
                host = qobject_cast<PluginHost *>(plugins->at(manifests.first().first).value("host").value<QObject *>());
                QQmlEngine::setObjectOwnership(host, QQmlEngine::CppOwnership);
                engine.globalObject().setProperty("pinhole", engine.newQObject(host));
                engine.globalObject().setProperty("said", engine.newArray());
            }
        }
        ~PluginBench() {
            plugins.reset();
            qunsetenv("PINHOLE_BUILTIN_PLUGINS");
            qunsetenv("PINHOLE_SOCKET");
        }
        // What plugins keep apart from their settings, and the tours seen:
        // none. Only ever the test mode's folders (~/.qttest).
        static void forgetKept() {
            QStandardPaths::setTestModeEnabled(true);
            for (const QString &path : {Plugins::stateRoot(), Plugins::cacheRoot(), Plugins::touredPath()})
                QVERIFY2(path.contains(QLatin1String("/.qttest/")), qPrintable(path));
            QDir(Plugins::stateRoot()).removeRecursively();
            QDir(Plugins::cacheRoot()).removeRecursively();
            QFile::remove(Plugins::touredPath());
        }
        QJSValue js(const QString &code) {
            const QJSValue result = engine.evaluate(code);
            if (result.isError())
                qWarning("%s", qPrintable(result.toString()));
            return result;
        }
        // What the JavaScript pushed onto `said`, in order.
        QStringList said() { return engine.globalObject().property("said").toVariant().toStringList(); }
    };

    // A grid whose mouse can be driven from a test.
    struct ClickableGrid : PhotoGrid {
        void press(QPointF at) {
            QMouseEvent e(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            mousePressEvent(&e);
        }
        void release(QPointF at) {
            QMouseEvent e(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            mouseReleaseEvent(&e);
        }
        void sideWheel(int angle) {
            const QPointF at(width() / 2, height() / 2);
            QWheelEvent event(at, at, QPoint(angle / 8, 0), QPoint(angle, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::NoScrollPhase, false);
            wheelEvent(&event);
        }
        // Moved with the button held.
        void move(QPointF at) {
            QMouseEvent e(QEvent::MouseMove, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            mouseMoveEvent(&e);
        }
    };

private slots:
    // A client that finds no service starts one: never a real pinholed
    // from a test, whose socket a fake service holds.
    void initTestCase() { qputenv("PINHOLED", "/bin/false"); }

    // Source dates stay intact: only presentation order changes. An older
    // photo received yesterday belongs at the newest end for thirty days.
    void timelineKeepsRecentAdditionsTogetherThenReturnsThemToCaptureOrder() {
        QTemporaryDir dir;
        const qint64 day = 86400000;
        qint64 now = QDateTime(QDate(2026, 11, 2), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        makeLibrary(dir.path(), {{"a", 1000}, {"b", 2000}, {"c", 3000}, {"d", 4000},
                                 {"e", 5000}, {"f", 6000}, {"g", 7000}, {"h", 8000}});
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "added-fixture");
            db.setDatabaseName(dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE assets ADD COLUMN added_date INTEGER"));
            const QList<qint64> dates{now - 2 * day, now - 45 * day, now - day, now - 31 * day,
                                      0, now - Timeline::recentWindow, now - 2 * day, now + day};
            for (int i = 0; i < dates.size(); ++i) {
                q.prepare("UPDATE assets SET added_date = ? WHERE id = ?");
                q.addBindValue(dates[i]);
                q.addBindValue(QString(QChar('a' + i)));
                QVERIFY(q.exec());
            }
        }
        QSqlDatabase::removeDatabase("added-fixture");
        Library library;
        QString error;
        QVERIFY2(library.open(dir.path(), &error), qPrintable(error));
        Timeline timeline(dir.path() + "/window.conf", nullptr, [&] { return now; });
        QCOMPARE(timeline.mode(), "smart");
        timeline.setLibrary(&library);
        QCOMPARE(timeline.order(), (std::vector<int>{1, 3, 4, 7, 5, 0, 6, 2}));
        QCOMPARE(library.at(0).id, QByteArray("a"));
        QCOMPARE(library.at(0).date, 1000LL);
        QCOMPARE(timeline.dateOf(0), now - 2 * day);
        QVERIFY(!timeline.recentlyAdded(7)); // future/invalid dates stay out
        QSignalSpy changed(&timeline, &Timeline::orderChanged);
        timeline.refresh();
        QCOMPARE(changed.count(), 0);
        ++now; // exactly thirty days is recent; one ms past it is archived
        timeline.refresh();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(timeline.order(), (std::vector<int>{1, 3, 4, 5, 7, 0, 6, 2}));
        timeline.setMode("added");
        QCOMPARE(timeline.order(), (std::vector<int>{4, 1, 3, 5, 0, 6, 2, 7}));
        QCOMPARE(timeline.dateOf(4), library.at(4).date); // missing added date
        Timeline remembered(dir.path() + "/window.conf");
        QCOMPARE(remembered.mode(), "added");
        timeline.setMode("taken");
        QCOMPARE(timeline.order(), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
        QCOMPARE(timeline.dateOf(0), library.at(0).date);
        timeline.setMode("unknown");
        QCOMPARE(timeline.mode(), "taken");
    }

    void gridAndViewerFollowTimelineWithoutChangingPluginPositions() {
        QTemporaryDir dir;
        const qint64 now = 1793620800000LL, day = 86400000;
        makeLibrary(dir.path(), {{"a", 1000}, {"b", 2000}, {"c", 3000}, {"d", 4000}});
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "grid-added");
            db.setDatabaseName(dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE assets ADD COLUMN added_date INTEGER"));
            const QList<qint64> dates{now - 2 * day, now - 60 * day, now - day, now - 50 * day};
            for (int i = 0; i < dates.size(); ++i) {
                q.prepare("UPDATE assets SET added_date = ? WHERE id = ?");
                q.addBindValue(dates[i]); q.addBindValue(QString(QChar('a' + i)));
                QVERIFY(q.exec());
            }
        }
        QSqlDatabase::removeDatabase("grid-added");
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        Timeline timeline(dir.path() + "/window.conf", nullptr, [=] { return now; });
        timeline.setLibrary(&library);
        ThumbnailLoader loader(&library, 1);
        PhotoGrid grid;
        grid.setWidth(600); grid.setHeight(500);
        grid.setSources(&library, &loader, nullptr);
        grid.setTimeline(&timeline);
        grid.setColumnsNow(3);
        QCOMPARE(timeline.order(), (std::vector<int>{1, 3, 0, 2}));
        const auto slot = [&](int photo) { const QRectF r = grid.rectOf(photo); return std::pair(r.top(), r.left()); };
        QVERIFY(slot(1) < slot(0));
        QVERIFY(slot(1) < slot(3));
        grid.setSelecting(true);
        grid.setCursorShown(true);
        grid.putCursorOn(0); grid.tickCursor();
        QCOMPARE(grid.selection(), QList<int>{0});
        timeline.setMode("taken");
        QCOMPARE(grid.selection(), QList<int>{0});
        QCOMPARE(grid.currentIndex(), 0);
        timeline.setMode("smart");
        ServiceClient service(dir.path());
        Viewer viewer(&library, &service);
        viewer.setTimeline(&timeline);
        QCOMPARE(viewer.photoAt(0), 1);
        QCOMPARE(viewer.photoAt(2), 0);
        viewer.openAt(0, {});
        viewer.step(1); QCOMPARE(viewer.index(), 2);
        viewer.step(-1); QCOMPARE(viewer.index(), 0);
        viewer.step(-1); QCOMPARE(viewer.index(), 3); // larger capture index, earlier grid position
        viewer.goTo(3); QCOMPARE(viewer.index(), 2);
        viewer.setHidden({0});
        QCOMPARE(viewer.count(), 3);
        viewer.openAt(0, {}); // a plugin may open a hidden photo directly
        viewer.step(1); QCOMPARE(viewer.index(), 2);
        viewer.openAt(0, {});
        viewer.step(-1); QCOMPARE(viewer.index(), 3);
        grid.setStacks({QVariant(QVariantList{1, 0})});
        grid.setHidden({2});
        QCOMPARE(grid.count(), 2);
        QCOMPARE(grid.stackOf(0), 0);
        QCOMPARE(grid.rectOf(0), grid.rectOf(1));
        QVERIFY(grid.rectOf(3).top() <= grid.rectOf(1).top());
        QVERIFY(grid.rectOf(3).left() < grid.rectOf(1).left());
        // Reload added dates without moving library positions or selections.
        grid.rememberAnchor();
        Library fresh;
        QVERIFY(fresh.open(dir.path(), &error));
        loader.reload([&] { library.swap(fresh); });
        timeline.libraryReloaded();
        grid.libraryReloaded({0, 1, 2, 3});
        viewer.libraryReloaded({0, 1, 2, 3});
        QCOMPARE(grid.stackOf(0), 0);
        QCOMPARE(viewer.index(), 3);
        QCOMPARE(library.at(0).id, QByteArray("a"));
    }

    void librariesWithoutAddedDatesKeepTheirCaptureOrder() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1000}, {"b", 2000}});
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        Timeline timeline(dir.path() + "/window.conf");
        timeline.setLibrary(&library);
        QCOMPARE(library.at(0).added, 0LL);
        QCOMPARE(timeline.order(), (std::vector<int>{0, 1}));
        timeline.setMode("added");
        QCOMPARE(timeline.order(), (std::vector<int>{0, 1}));
        QCOMPARE(timeline.dateOf(1), 2000LL);
    }

    // Real asynchronous process replies: checking cannot install, repeated
    // clicks cannot start a second build, and a failed build stays retryable.
    void updaterChecksThenBuildsOnlyWhenAsked() {
        QTemporaryDir prefix;
        QVERIFY(prefix.isValid());
        const auto before = qgetenv("PINHOLE_INSTALL_PREFIX");
        const auto restored = qScopeGuard([&] {
            if (before.isNull()) qunsetenv("PINHOLE_INSTALL_PREFIX");
            else qputenv("PINHOLE_INSTALL_PREFIX", before);
        });
        qputenv("PINHOLE_INSTALL_PREFIX", prefix.path().toUtf8());
        QDir().mkpath(prefix.path() + "/bin");
        QDir().mkpath(prefix.path() + "/share/pinhole");
        write(prefix.path() + "/share/pinhole/update.json", R"({"mode":"developer"})");
        const QString helper = prefix.path() + "/bin/pinhole-update";
        write(helper, R"(#!/bin/sh
if [ "$1" = --check ]; then
  printf '%s\n' '{"available":true,"revision":"1234567890"}'
else
  printf '%s\n' '{"revision":"1234567890","log":"/tmp/pinhole-update-test.log"}'
fi
)");
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Updater updater;
        QVERIFY(updater.supported());
        QCOMPARE(updater.state(), "idle");
        updater.apply();
        QCOMPARE(updater.state(), "idle");
        updater.check();
        QCOMPARE(updater.state(), "checking");
        QTRY_COMPARE(updater.state(), "available");
        QCOMPARE(updater.revision(), "12345678");
        updater.apply();
        QCOMPARE(updater.state(), "updating");
        updater.apply();
        QTRY_COMPARE(updater.state(), "ready");
        QCOMPARE(updater.logPath(), "/tmp/pinhole-update-test.log");
        updater.check();
        QCOMPARE(updater.state(), "ready");
    }

    void updaterBuildFailureKeepsRunningAndCanBeCheckedAgain() {
        QTemporaryDir prefix;
        const auto before = qgetenv("PINHOLE_INSTALL_PREFIX");
        const auto restored = qScopeGuard([&] {
            if (before.isNull()) qunsetenv("PINHOLE_INSTALL_PREFIX");
            else qputenv("PINHOLE_INSTALL_PREFIX", before);
        });
        qputenv("PINHOLE_INSTALL_PREFIX", prefix.path().toUtf8());
        QDir().mkpath(prefix.path() + "/bin");
        QDir().mkpath(prefix.path() + "/share/pinhole");
        write(prefix.path() + "/share/pinhole/update.json", R"({"mode":"developer"})");
        const QString helper = prefix.path() + "/bin/pinhole-update";
        write(helper, R"(#!/bin/sh
printf '%s\n' '{"error":"network"}'
exit 1
)");
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Updater updater;
        updater.check();
        QTRY_COMPARE(updater.state(), "error");
        QCOMPARE(updater.error(), "network");
        write(helper, R"(#!/bin/sh
printf '%s\n' '{"available":false,"revision":"1234567890"}'
)");
        updater.check();
        QTRY_COMPARE(updater.state(), "current");
        QVERIFY(updater.error().isEmpty());
    }


    void updaterIsAbsentForPackagesAndCopiedInstalls() {
        QTemporaryDir prefix;
        const auto before = qgetenv("PINHOLE_INSTALL_PREFIX");
        const auto restored = qScopeGuard([&] {
            if (before.isNull()) qunsetenv("PINHOLE_INSTALL_PREFIX");
            else qputenv("PINHOLE_INSTALL_PREFIX", before);
        });
        qputenv("PINHOLE_INSTALL_PREFIX", prefix.path().toUtf8());
        QDir().mkpath(prefix.path() + "/bin");
        QDir().mkpath(prefix.path() + "/share/pinhole");
        const QString helper = prefix.path() + "/bin/pinhole-update";
        write(helper, "#!/bin/sh\nexit 99\n");
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        for (const QString &mode : {QStringLiteral("package"), QStringLiteral("local")}) {
            write(prefix.path() + "/share/pinhole/update.json", ("{\"mode\":\"" + mode + "\"}").toUtf8());
            Updater updater;
            QVERIFY(!updater.supported());
            QCOMPARE(updater.state(), "unavailable");
            updater.check();
            updater.apply();
            QCOMPARE(updater.state(), "unavailable");
        }
    }

    void updaterTracksDetachedWorkerAndRejectsMissingRestartBinary() {
        QTemporaryDir prefix;
        const auto before = qgetenv("PINHOLE_INSTALL_PREFIX");
        const auto restored = qScopeGuard([&] {
            if (before.isNull()) qunsetenv("PINHOLE_INSTALL_PREFIX");
            else qputenv("PINHOLE_INSTALL_PREFIX", before);
        });
        qputenv("PINHOLE_INSTALL_PREFIX", prefix.path().toUtf8());
        QDir().mkpath(prefix.path() + "/bin");
        QDir().mkpath(prefix.path() + "/share/pinhole");
        write(prefix.path() + "/share/pinhole/update.json", R"({"mode":"developer"})");
        const QString helper = prefix.path() + "/bin/pinhole-update";
        const QString job = prefix.path() + "/job.json";
        write(job, QJsonDocument(QJsonObject{{"state", "updating"}, {"pid", QCoreApplication::applicationPid()}}).toJson());
        const QByteArray reply = QJsonDocument(QJsonObject{{"state", "updating"}, {"job", job}}).toJson(QJsonDocument::Compact);
        write(helper, "#!/bin/sh\nprintf '%s\\n' '" + reply + "'\n");
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Updater updater;
        updater.check();
        QTRY_COMPARE(updater.state(), "updating");
        updater.check();
        QCOMPARE(updater.state(), "updating");
        write(job, R"({"state":"ready","revision":"1234567890"})");
        QTRY_COMPARE(updater.state(), "ready");
        updater.restart();
        QCOMPARE(updater.state(), "error");
        QCOMPARE(updater.error(), "restart");
    }

    // Undo and redo, as Ctrl+Z and Ctrl+⇧Z take them.
    void historyUndoesAndRedoesInTurn() {
        History history;
        int value = 0;
        QStringList said;
        connect(&history, &History::undone, this, [&](const QString &name) { said << "undone " + name; });
        connect(&history, &History::redone, this, [&](const QString &name) { said << "redone " + name; });
        connect(&history, &History::nothing, this, [&](bool undo) { said << (undo ? "no undo" : "no redo"); });
        auto set = [&](const QString &owner, int from, int to) {
            value = to;
            history.did(owner, QString::number(to), [&, from] { value = from; }, [&, to] { value = to; });
        };
        set("series", 0, 1);
        set({}, 1, 2);
        set("series", 2, 3);
        history.undo();
        history.undo();
        QCOMPARE(value, 1);
        history.redo();
        QCOMPARE(value, 2);
        // A new step drops what was undone.
        set("series", 2, 4);
        QVERIFY(!history.canRedo());
        history.redo();
        history.undo();
        QCOMPARE(value, 2);
        QCOMPARE(said, QStringList({"undone 3", "undone 2", "redone 2", "no redo", "undone 4"}));
    }
    void historyDoesNotRecordWhatAnUndoDoes() {
        History history;
        int steps = 0;
        connect(&history, &History::changed, this, [&] { ++steps; });
        history.did({}, "a", [&] { history.did({}, "inside", [] {}, [] {}); }, [] {});
        history.undo();
        QVERIFY(!history.canUndo());
        QVERIFY(history.canRedo());
    }
    void aPluginLoadedAgainTakesOnlyItsOwnSteps() {
        History history;
        QStringList undone;
        connect(&history, &History::undone, this, [&](const QString &name) { undone << name; });
        history.did("series", "s1", [] {}, [] {});
        history.did({}, "favourite", [] {}, [] {});
        history.did("series", "s2", [] {}, [] {});
        history.did({}, "unfavourite", [] {}, [] {});
        history.undo();
        history.forget("series");
        history.undo();
        QVERIFY(!history.canUndo());
        QVERIFY(history.canRedo());
        QCOMPARE(undone, QStringList({"unfavourite", "favourite"}));
    }

    void aFlickCoastsAboutHalfASecondOfItsSpeed() {
        ScrollPhysics p = flicked(20, 12);
        const double start = p.position(), v = p.velocity();
        QVERIFY(v > 1000);
        settle(p);
        // AppKit's 0.998 per ms: the distance is the speed times 0.5 s.
        QVERIFY(std::abs((p.position() - start) - v * 0.5) < v * 0.02);
    }

    void aSlowReleaseDoesNotCoast() {
        ScrollPhysics p = flicked(1, 12);
        QCOMPARE(p.velocity(), 0.0);
        QVERIFY(!p.animating());
    }

    void fingersThatStoppedBeforeLiftingDoNotThrow() {
        ScrollPhysics p;
        p.setBounds(0, 1e9, 1000);
        p.begin(0);
        for (int i = 1; i <= 12; ++i)
            p.move(i * 8, 20);
        p.end(96 + 200);
        QVERIFY(!p.animating());
    }

    void aTouchStopsACoastAndAFlickBuildsOnIt() {
        ScrollPhysics p = flicked(20, 12);
        const double v = p.velocity();
        p.step(0.05);
        p.touch(1000);
        QVERIFY(!p.animating());
        // The touch turns into another flick the same way: faster than either.
        p.begin(1010);
        for (int i = 1; i <= 12; ++i)
            p.move(1010 + i * 8, 20);
        p.end(1010 + 13 * 8);
        QVERIFY(p.velocity() > v * 1.3);
    }

    void theEndsStretchAndSpringBack() {
        ScrollPhysics p;
        p.setBounds(0, 10000, 1000);
        p.jumpTo(0);
        p.begin(0);
        for (int i = 1; i <= 30; ++i)
            p.move(i * 8, -20);
        // Pulled 600 px past the top, it gives less: the rubber band.
        QVERIFY(p.position() < 0 && p.position() > -600);
        p.end(30 * 8 + 300);
        settle(p);
        QCOMPARE(p.position(), 0.0);
    }

    void aFastCoastBouncesOnPastTheEnd() {
        // Coasting fast into the end, or lifted fast past it: the grid and
        // every panel alike (a Scroller has no gentler tuning), never
        // further than a quarter of the view (or the drag's own stretch),
        // and it comes back.
        for (const bool lifted : {false, true}) {
            ScrollPhysics p;
            p.setBounds(0, 3000, 1000);
            p.jumpTo(lifted ? 2900 : 0);
            p.begin(0);
            for (int i = 1; i <= 6; ++i)
                p.move(i * 8, 30);
            p.end(6 * 8 + 8);
            // How much further than where the fingers left it.
            const double atLift = std::max(0.0, p.position() - 3000);
            double most = atLift;
            for (int i = 0; i < 2000 && p.animating(); ++i) {
                p.step(1.0 / 120);
                most = std::max(most, p.position() - 3000);
            }
            // Coasting, on past the end; lifted already stretched past a
            // quarter, no further than the fingers took it.
            if (!lifted)
                QVERIFY2(most > 150, qPrintable(QString::number(most)));
            QVERIFY2(most <= std::max(atLift, 250.0) + 0.5, qPrintable(QStringLiteral("%1, lifted at %2").arg(most).arg(atLift)));
            QCOMPARE(p.position(), 3000.0);
        }
        // The fastest flick into the end of a short panel (916 px): once
        // 1,435 px past it, all its rows out of sight; now a quarter of it.
        for (const double extent : {916.0, 3200.0}) {
            ScrollPhysics p;
            p.setBounds(0, 20000, extent);
            p.jumpTo(10000);
            p.fling(40000);
            double most = 0;
            for (int i = 0; i < 2000 && p.animating(); ++i) {
                p.step(1.0 / 120);
                most = std::max(most, p.position() - 20000);
            }
            // The tall one's slower bounce as it was, under its quarter.
            QVERIFY2((extent > 1000 || most > 0.2 * extent) && most <= 0.25 * extent + 0.5,
                     qPrintable(QStringLiteral("%1 past a view of %2").arg(most).arg(extent)));
            QCOMPARE(p.position(), 20000.0);
        }
    }

    void aWheelNotchStopsAtTheEnd() {
        ScrollPhysics p;
        p.setBounds(0, 1000, 1000);
        p.jumpTo(950);
        p.nudge(200);
        settle(p);
        QCOMPARE(p.position(), 1000.0);
    }

    void aSlowlyTurnedWheelGlidesWithoutPulsing() {
        // A wheel turned slowly: a step every 40 ms, watched at 120 Hz. The
        // speed from frame to frame barely changes, and each step moves
        // about its whole distance.
        ScrollPhysics p;
        p.setBounds(0, 1e9, 1000);
        p.jumpTo(500000);
        double last = p.position(), lowest = 1e9, highest = 0;
        for (int frame = 0; frame < 120; ++frame) {
            const double t = frame * 1000.0 / 120;
            if (frame % 5 == 0 && frame < 100)
                p.wheel(t, 20);
            p.step(1.0 / 120);
            if (frame >= 40 && frame < 100) {
                lowest = std::min(lowest, p.position() - last);
                highest = std::max(highest, p.position() - last);
            }
            last = p.position();
        }
        QVERIFY(lowest > 0);
        QVERIFY(highest < lowest * 1.4);
        settle(p);
        QVERIFY(p.position() > 500000.0 + 20 * 20 * 0.9 && p.position() <= 500000.0 + 20 * 20);
    }

    void aWheelClickMovesItsWholeStep() {
        ScrollPhysics p;
        p.setBounds(0, 1e9, 1000);
        p.jumpTo(500000);
        p.wheel(0, 75);
        settle(p);
        QCOMPARE(p.position(), 500075.0);
    }

    void aWheelSpinGathersSpeedLikeShift() {
        // An MX Master spinning freely: a notch every 7 ms for 4 s. It
        // starts at about the cruise speed, however fast the notches come,
        // and gets to the top speed by the end.
        ScrollPhysics p;
        p.setBounds(0, 1e9, 1000);
        p.jumpTo(500000);
        auto speedAt = [&](double fromMs, double toMs) {
            const double from = p.position();
            for (double t = fromMs; t < toMs; t += 7) {
                p.wheel(t, 75);
                p.step(0.007);
            }
            return (p.position() - from) / (toMs - fromMs) * 1000;
        };
        speedAt(0, 100);
        const double early = speedAt(100, 300);
        speedAt(300, 3400);
        const double late = speedAt(3400, 4000);
        const ScrollPhysics::Params defaults;
        QVERIFY(early > defaults.wheelCruise * 0.7 && early < defaults.wheelCruise * 1.3);
        QVERIFY(late > defaults.wheelTopSpeed * 0.85 && late < defaults.wheelTopSpeed * 1.15);
    }

    void aWheelSpinStartsOverAfterAPause() {
        ScrollPhysics p;
        p.setBounds(0, 1e9, 1000);
        p.jumpTo(500000);
        double t = 0;
        for (; t < 3000; t += 7)
            p.wheel(t, 75);
        settle(p);
        // After a pause the first click is a click again.
        const double before = p.position();
        p.wheel(t + 500, 75);
        settle(p);
        QCOMPARE(p.position() - before, 75.0);
    }

    void aWheelStopsAtTheEnd() {
        ScrollPhysics p;
        p.setBounds(0, 1000, 1000);
        p.jumpTo(950);
        for (int i = 0; i < 8; ++i)
            p.wheel(i * 5, 30);
        settle(p);
        QCOMPARE(p.position(), 1000.0);
    }

    void themeFallsBackAndReadsTheAccent() {
        QTemporaryDir dir;
        {
            Theme theme(dir.path());
            QCOMPARE(theme.accent(), QString("#FFD60A"));
        }
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#7aa2f7\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#7aa2f7"));
    }

    // Every colour of colors.toml, the mode, and in-between shades a theme
    // leaves out made from its background and foreground.
    void themeReadsEveryColourAndTheMode() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#205EA6\"\nbackground = \"#FFFCF0\"\nforeground = \"#100F0F\"\n"
              "dark_foreground = \"#878580\"\n");
        write(dir.path() + "/theme/shell.toml", "[font]\nbase-size = 14\n");
        Theme theme(dir.path());
        QVERIFY(theme.light());
        QCOMPARE(theme.background(), QColor("#FFFCF0"));
        QCOMPARE(theme.foreground(), QColor("#100F0F"));
        QCOMPARE(theme.darkForeground(), QColor("#878580"));
        QCOMPARE(theme.fontSize(), 14);
        // Not in the file: between background and foreground, near the
        // background, and on the light side for a light theme's darker one.
        QVERIFY(theme.lighterBackground() != theme.background());
        QVERIFY(theme.lighterBackground().lightnessF() > 0.8);
        QVERIFY(theme.darkBackground().lightnessF() > 0.8);
    }

    // The text size set in Omarchy's settings wins over the theme's, and a
    // change to it shows without a restart.
    void themeTakesTheTextSizeFromOmarchysSettings() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QDir(dir.path()).mkpath("config");
        write(dir.path() + "/theme/colors.toml", "accent = \"#7aa2f7\"\n");
        write(dir.path() + "/theme/shell.toml", "[font]\n# base-size = 20\nbase-size = 12\n");
        write(dir.path() + "/config/shell.toml", "[font]\nbase-size = 16\n");
        Theme theme(dir.path());
        QCOMPARE(theme.fontSize(), 16);
        write(dir.path() + "/config/shell.toml", "[font]\nbase-size = 18\n");
        QTRY_COMPARE(theme.fontSize(), 18);
        QFile::remove(dir.path() + "/config/shell.toml");
        QTRY_COMPARE(theme.fontSize(), 12);
    }

    void polishHasItsThreePluralForms() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pl\n");
        Translator translator(dir.path() + "/language", PINHOLE_I18N);
        QCOMPARE(translator.language(), QString("pl"));
        QCOMPARE(translator.translate("Outgoing", "Copied %n photo(s)", nullptr, 1), QString("Skopiowano %n zdjęcie"));
        QCOMPARE(translator.translate("Outgoing", "Copied %n photo(s)", nullptr, 3), QString("Skopiowano %n zdjęcia"));
        QCOMPARE(translator.translate("Outgoing", "Copied %n photo(s)", nullptr, 5), QString("Skopiowano %n zdjęć"));
        QCOMPARE(translator.translate("Outgoing", "Copied %n photo(s)", nullptr, 22), QString("Skopiowano %n zdjęcia"));
        QCOMPARE(translator.translate("Main", "Quit"), QString("Zakończ"));
        QCOMPARE(translator.translate("Incoming", "Adding %n file(s) to %1…", nullptr, 1), QString("Dodawanie %n pliku do %1…"));
        QCOMPARE(translator.translate("Incoming", "Added %n file(s) to %1", nullptr, 3), QString("Dodano %n pliki do %1"));
        QCOMPARE(translator.translate("Incoming", "%n file(s) not added", nullptr, 5), QString("Nie dodano %n plików"));
    }

    void englishIsTheFallback() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", PINHOLE_I18N);
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
        QCOMPARE(translator.translate("Outgoing", "Copied %n photo(s)", nullptr, 1), QString("Copied %n photo"));
        QCOMPARE(translator.translate("Incoming", "Added %n file(s) to %1", nullptr, 2), QString("Added %n files to %1"));
    }

    // The English each qsTr() or tr() in a folder's files says, and the
    // file it is first in. A C++ raw string is another file's words (the
    // Main.qml `pinhole plugin new` writes), and a plugin's own checks are
    // not shown.
    static QMap<QString, QString> saidIn(const QString &folder, const QStringList &patterns) {
        // (Plain literals: moc takes a raw one with a quote in it for its end.)
        static const QRegularExpression said(QStringLiteral("\\b(?:qsTr|tr|QT_TR_NOOP)\\s*\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
        static const QRegularExpression raw(QStringLiteral("\\bR\"([^(\\s]*)\\(.*?\\)\\1\""),
                                            QRegularExpression::DotMatchesEverythingOption);
        QMap<QString, QString> out;
        QDirIterator files(folder, patterns, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            const QString file = QDir(folder).relativeFilePath(path);
            if (file.startsWith("check/") || file.startsWith("tests/") || file.contains("target/") || file.contains("build/"))
                continue;
            QString text = read(path);
            // A comment's qsTr("Save") is an example.
            static const QRegularExpression comment(QStringLiteral("^\\s*//.*$"), QRegularExpression::MultilineOption);
            text.remove(comment);
            if (!path.endsWith(".qml") && !path.endsWith(".js"))
                text.remove(raw);
            for (auto m = said.globalMatch(text); m.hasNext();) {
                const QString english = QJsonDocument::fromJson("[\"" + m.next().captured(1).toUtf8() + "\"]").array().at(0).toString();
                if (!out.contains(english))
                    out.insert(english, file);
            }
        }
        return out;
    }

    // Every string Pinhole and its plugins say has its Polish: a plugin's
    // in its own catalog or else Pinhole's, as Translator looks it up, a
    // counted one in each of its forms; and no catalog keeps words for
    // English nothing says any more. A plugin's name and description are
    // its words too.
    void everyStringHasItsPolishAndNoneIsLeftOver() {
        const QString app = QStringLiteral(PINHOLE_I18N "/..");
        const QString builtIn = QStringLiteral(PINHOLE_I18N "/../../../plugins");
        const auto catalog = [](const QString &folder, const QString &language) {
            return QJsonDocument::fromJson(read(folder + "/i18n/" + language + ".json").toUtf8()).object();
        };
        QStringList wrong;
        const auto polishOf = [&](const QString &english, const QJsonValue &polish, const QString &where) {
            if (polish.isUndefined())
                wrong << where + ": no Polish for \"" + english + "\"";
            else if ((english.contains("%n") || english.contains("%Ln"))
                     && !(polish.isObject() && polish.toObject().contains("one") && polish.toObject().contains("few")
                          && polish.toObject().contains("many")))
                wrong << where + ": \"" + english + "\" is counted, and its Polish has not its one, few and many";
        };
        const auto leftOver = [&](const QJsonObject &strings, const QSet<QString> &used, const QString &where) {
            for (auto it = strings.begin(); it != strings.end(); ++it)
                if (!used.contains(it.key()))
                    wrong << where + ": \"" + it.key() + "\" is said nowhere";
        };
        const QJsonObject pinhole = catalog(app, "pl");
        const QMap<QString, QString> pinholeSays = saidIn(app, {"*.qml", "*.js", "*.cpp", "*.h"});
        for (auto it = pinholeSays.begin(); it != pinholeSays.end(); ++it)
            polishOf(it.key(), pinhole.value(it.key()), "app/src/" + it.value());
        QSet<QString> pinholeUsed(pinholeSays.keyBegin(), pinholeSays.keyEnd());
        const QFileInfoList plugins = QDir(builtIn).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        QVERIFY(plugins.size() > 5);
        for (const QFileInfo &plugin : plugins) {
            // A folder with no manifest is no plugin, as Plugins::scan has
            // it: iCloud's and folders' Rust before their plugins' QML.
            if (!QFileInfo::exists(plugin.filePath() + "/manifest.json"))
                continue;
            const QJsonObject own = catalog(plugin.filePath(), "pl");
            const QJsonObject manifest = QJsonDocument::fromJson(read(plugin.filePath() + "/manifest.json").toUtf8()).object();
            const QMap<QString, QString> says = saidIn(plugin.filePath(), {"*.qml", "*.js"});
            QSet<QString> used(says.keyBegin(), says.keyEnd());
            for (const char *field : {"name", "description"}) {
                const QString english = manifest.value(field).toString();
                polishOf(english, own.value(english), "plugins/" + plugin.fileName() + "/manifest.json");
                used << english;
            }
            for (auto it = says.begin(); it != says.end(); ++it) {
                const bool itsOwn = own.contains(it.key());
                if (!itsOwn)
                    pinholeUsed << it.key();
                polishOf(it.key(), itsOwn ? own.value(it.key()) : pinhole.value(it.key()), "plugins/" + plugin.fileName() + "/" + it.value());
            }
            leftOver(own, used, "plugins/" + plugin.fileName() + "/i18n/pl.json");
            leftOver(catalog(plugin.filePath(), "en"), used, "plugins/" + plugin.fileName() + "/i18n/en.json");
        }
        leftOver(pinhole, pinholeUsed, "app/src/i18n/pl.json");
        leftOver(catalog(app, "en"), QSet<QString>(pinholeSays.keyBegin(), pinholeSays.keyEnd()), "app/src/i18n/en.json");
        QVERIFY2(wrong.isEmpty(), qPrintable("\n" + wrong.join("\n")));
    }

    // Two plugins and Pinhole giving the same English other words: each
    // string in the catalog of the files it is in, Pinhole's in its own
    // alone; in a binding or in JavaScript, in a main file (each plugin
    // has its Main.qml) or another of its own.
    void eachCatalogKeepsItsOwnWordsForTheSameEnglish() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pl\n");
        for (const auto &[name, click] : {std::pair<QString, QString>{"a", "Kliknięcie"}, {"b", "Klik"}}) {
            const QString other = name == "a" ? "Finder" : "Days";
            QDir(dir.path()).mkpath(name + "/i18n");
            write(dir.path() + "/" + name + "/i18n/pl.json",
                  QJsonDocument(QJsonObject{{"Click", click}, {"Frame", name.toUpper()}}).toJson());
            write(dir.path() + "/" + name + "/Main.qml", QStringLiteral(R"(import QtQuick
QtObject {
    property string bound: qsTr("Click")
    property string called: word()
    function word() { return qsTr("Click") }
    property string common: qsTr("Quit")
    property QtObject other: %1 {}
}
)").arg(other).toUtf8());
            write(dir.path() + "/" + name + "/" + other + ".qml",
                  "import QtQuick\nQtObject { property string bound: qsTr(\"Click\"); property string called: [qsTr(\"Click\")][0] }\n");
        }
        Translator translator(dir.path() + "/language", PINHOLE_I18N);
        translator.setPluginCatalogs({{"me.a", dir.path() + "/a"}, {"me.b", dir.path() + "/b"}});
        QQmlEngine engine;
        translator.setEngine(&engine);
        QCoreApplication::installTranslator(&translator);
        // Pinhole's own: its Polish whatever the plugins say.
        QCOMPARE(translator.translate("HelpOverlay", "Click"), QString("Kliknij"));
        QCOMPARE(translator.translate("Main", "Click"), QString("Kliknij"));
        QCOMPARE(translator.translate("Outgoing", "Click"), QString("Kliknij"));
        for (const auto &[name, click] : {std::pair<QString, QString>{"a", "Kliknięcie"}, {"b", "Klik"}}) {
            // Loaded as Plugins loads a plugin: its id in the URL.
            const QUrl url(QUrl::fromLocalFile(dir.path() + "/" + name + "/Main.qml").toString() + "?plugin=me." + name + "&load=0");
            QQmlComponent component(&engine, url);
            std::unique_ptr<QObject> plugin(component.create());
            QVERIFY2(plugin, qPrintable(component.errorString()));
            QCOMPARE(plugin->property("bound").toString(), click);
            QCOMPARE(plugin->property("called").toString(), click);
            QObject *other = plugin->property("other").value<QObject *>();
            QCOMPARE(other->property("bound").toString(), click);
            QCOMPARE(other->property("called").toString(), click);
            // What it does not say itself, Pinhole's.
            QCOMPARE(plugin->property("common").toString(), QString("Zakończ"));
            // Its name, from its own catalog.
            QCOMPARE(translator.pluginText("me." + name, "Frame"), name.toUpper());
            QCOMPARE(translator.pluginText("me." + name, "Not said"), QString("Not said"));
        }
        QCoreApplication::removeTranslator(&translator);
    }

    // A plugin's name and description in the language, from its catalog.
    void aPluginsNameAndDescriptionAreItsCatalogs() {
        PluginBench bench({{"t", {{"name", "Colours"}, {"description", "Finds photos by colour."}}}});
        QCOMPARE(bench.plugins->at("t").value("name").toString(), QString("Colours"));
        QDir(bench.dir.path() + "/plugins/t").mkdir("i18n");
        write(bench.dir.path() + "/plugins/t/i18n/pl.json",
              QJsonDocument(QJsonObject{{"Colours", "Kolory"}, {"Finds photos by colour.", "Szuka zdjęć po kolorze."}}).toJson());
        QSignalSpy changed(bench.plugins.get(), &Plugins::changed);
        write(bench.dir.path() + "/language", "pl\n");
        QTRY_COMPARE(bench.plugins->at("t").value("name").toString(), QString("Kolory"));
        QCOMPARE(bench.plugins->at("t").value("description").toString(), QString("Szuka zdjęć po kolorze."));
        QVERIFY(changed.size() > 0);
    }

    // Who covers the photos, as every plugin sees it.
    void everyPluginSeesWhichOneCovers() {
        PluginBench bench({{"t", {}}, {"u", {}}});
        QSignalSpy seen(bench.host, &PluginHost::coveredByChanged);
        QCOMPARE(bench.js("pinhole.coveredBy").toString(), QString());
        bench.plugins->setCovering({"u"});
        QCOMPARE(bench.js("pinhole.coveredBy").toString(), QString("u"));
        QCOMPARE(seen.size(), 1);
        bench.plugins->setCovering({"u"});
        QCOMPARE(seen.size(), 1);
        // One beneath it: the top is still u's.
        bench.plugins->setCovering({"t", "u"});
        QCOMPARE(seen.size(), 1);
        bench.plugins->setCovering({});
        QCOMPARE(bench.js("pinhole.coveredBy").toString(), QString());
    }

    // The one over the photos handing a photo over closes those beneath it;
    // one beneath, or not covering, closes none.
    void aPhotoHandedOverFromTheTopClosesThoseBeneath() {
        PluginBench bench({{"t", {}}, {"u", {}}, {"v", {}}});
        auto hostOf = [&](const QString &id) {
            return qobject_cast<PluginHost *>(bench.plugins->at(id).value("host").value<QObject *>());
        };
        QSignalSpy t(hostOf("t"), &PluginHost::dismissed), u(hostOf("u"), &PluginHost::dismissed),
            v(hostOf("v"), &PluginHost::dismissed);
        bench.plugins->setCovering({"t", "u"});
        bench.plugins->handedOver("t");
        bench.plugins->handedOver("v");
        QCOMPARE(t.size() + u.size() + v.size(), 0);
        bench.plugins->handedOver("u");
        QCOMPARE(t.size(), 1);
        QCOMPARE(u.size(), 0);
        QCOMPARE(v.size(), 0);
    }

    // Each photo's local day, before 1970 too; a date of 0 is none.
    void aPhotosDayIsItsLocalDayBefore1970Too() {
        const auto at = [](int y, int m, int d, int h = 12) {
            return QDateTime(QDate(y, m, d), QTime(h, 0)).toMSecsSinceEpoch();
        };
        // In the library's order, by date. Half a second before a local
        // midnight before 1970 is the day before, not the day the second
        // rounded towards 1970 is in.
        PluginBench bench({{"t", {}}},
                          {{"none", 0}, {"scan", at(1965, 6, 15)}, {"leap", at(1960, 3, 1, 0) - 500}, {"now", at(2025, 1, 8)}});
        QCOMPARE(bench.js("pinhole.library.field('day').join(' ')").toString(), QString("19600229 19650615 0 20250108"));
    }

    void checkableBuiltinShellButtonsRequireApi7WithoutClaimingCustomComponents() {
        QTemporaryDir dir;
        const QString main = dir.path() + "/Main.qml";
        const auto check = [&](const QByteArray &code, bool found) {
            write(main, code);
            QCOMPARE(Plugins::apiUses(dir.path()).value(7).contains("ShellButton.checkable"), found);
        };
        check("import Pinhole.Plugin\nShellButton { checkable: true; active: true }\n", true);
        check("import Pinhole.Plugin 1.0 as P\nP.ShellButton { checkable: selected }\n", true);
        check("import Pinhole.Plugin\nShellButton { QtObject { property bool checkable: true } }\n", false);
        check("import Pinhole.Plugin\nOther.ShellButton { checkable: true }\n", false);
        check("import Pinhole.Plugin\nOther . ShellButton { checkable: true }\n", false);
        check("import Pinhole.Plugin\nShellButton { property bool checkable: true }\n", false);
        check("import Other.Controls\nShellButton { checkable: true }\n", false);
        check("import Pinhole.Plugin\nItem { property string words: 'ShellButton { checkable: true }' }\n"
              "// ShellButton { checkable: true }\n", false);
        check("import Pinhole.Plugin\nItem { component ShellButton: Item { property bool checkable: false }; "
              "ShellButton { checkable: true } }\n", false);
        write(dir.path() + "/ShellButton.qml", "import QtQuick\nItem { property bool checkable: false }\n");
        check("import Pinhole.Plugin\nShellButton { checkable: true }\n", false);
        // A qualified builtin stays unambiguous even alongside a local type.
        check("import Pinhole.Plugin as P\nP.ShellButton { checkable: true }\n", true);
        QFile::remove(dir.path() + "/ShellButton.qml");
        QDir().mkpath(dir.path() + "/controls");
        write(dir.path() + "/controls/ShellButton.qml", "import QtQuick\nItem { property bool checkable: false }\n");
        check("import Pinhole.Plugin\nimport \"controls\"\nShellButton { checkable: true }\n", false);
        check("import Pinhole.Plugin\nimport \"controls\" as C\nShellButton { checkable: true }\n", true);
    }

    // What came with API 2, by a plugin's code, comments and strings
    // aside; and each of Pinhole's own that uses any asks for API 2.
    void whatAPluginUsesOfApi2IsFoundAndPinholesOwnAskForIt() {
        QTemporaryDir dir;
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
// avgColor, pinhole.settings: only in a comment
PinholePlugin {
    id: plugin
    hidden: []
    property string colour: pinhole.library.item(0).avgColor /* .then( */
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()).value(2), QStringList({"hidden", "avgColor"}));
        write(dir.path() + "/Main.qml", "import QtQuick\nQtObject {}\n");
        write(dir.path() + "/Helper.js", "function go() { pinhole.run('x', []).then(r => r) }\n");
        QCOMPARE(Plugins::apiUses(dir.path()).value(2), QStringList({"promises"}));
        // Its own names and words are not the API's: a child's hidden, its
        // own covered and output, a URL's // and what follows, a string's
        // or a regular expression's words, a template's.
        QFile::remove(dir.path() + "/Helper.js");
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    id: plugin
    property bool covered: false
    property string site: "https://example.com/pinhole.notify" // hidden: [1]
    property string said: 'pinhole.settings: hidden: output: .then('
    property var pattern: /pinhole\.warn\/"x/
    property real half: width / 2 / 3
    Rectangle { hidden: true; property var output: [] }
    QtObject { id: other; property bool hidden: false }
    function f(n) { other.hidden = n; return `${n} pinhole.api .avgColor ${"`"}` }
    function g(o) { return o.run("x", [], { output: l => l }) }
    Connections { target: grid; function onHiddenChanged() {} function onCoveredByChanged() {} }
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()).value(2), QStringList());
        // As they are used: by the root's id, in a template's code, run's
        // options, a Connections to pinhole as a plugin's part has it.
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin as P
P.PinholePlugin {
    id: plugin
    function f(n) { plugin.hidden = [n]; return `${pinhole.library.item(n).avgColor}` }
    function g() { pinhole.run("x", [], { timeout: 10, output: line => line }) }
}
)");
        write(dir.path() + "/Part.qml", R"(import QtQuick
Item {
    property var host
    Connections { target: part.host.pinhole; function onCoveredByChanged() {} }
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()).value(2), QStringList({"pinhole.coveredBy", "hidden", "avgColor", "run's output"}));
        // A value's ++ or -- then a /: a division, not a regular expression
        // hiding the rest of the line.
        QFile::remove(dir.path() + "/Part.qml");
        for (const QString &step : {QStringLiteral("i++"), QStringLiteral("a[i]--"), QStringLiteral("(i)++")}) {
            write(dir.path() + "/Main.qml", QStringLiteral(R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    function f(i, a) { return %1 / 2 + pinhole.library.item(i).avgColor }
}
)").arg(step).toUtf8());
            QCOMPARE(Plugins::apiUses(dir.path()).value(2), QStringList({"avgColor"}));
        }
        // Pinhole's own: each that uses any asks for API 2, and what some
        // use is found.
        const QString builtIn = QStringLiteral(PINHOLE_I18N "/../../../plugins");
        for (const QFileInfo &plugin : QDir(builtIn).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile manifest(plugin.filePath() + "/manifest.json");
            if (!manifest.exists())
                continue; // no plugin, as Plugins::scan has it
            QVERIFY(manifest.open(QIODevice::ReadOnly));
            const int api = QJsonDocument::fromJson(manifest.readAll()).object().value("api").toInt(1);
            const QMap<int, QStringList> uses = Plugins::apiUses(plugin.filePath());
            for (auto it = uses.begin(); it != uses.end(); ++it)
                QVERIFY2(api >= it.key(), qPrintable(QStringLiteral("%1 uses %2, of API %3, and asks for API %4")
                                                         .arg(plugin.fileName(), it.value().join(", "))
                                                         .arg(it.key())
                                                         .arg(api)));
        }
        // A mark is API 3's, as its PluginMark says, a comment's not.
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
// PluginMark { }
PinholePlugin {
    marks: [ PluginMark { name: "x"; field: "live"; icon: "live" } ]
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()), (QMap<int, QStringList>{{3, {"marks"}}}));
        // So are its state and its cache; its cacheDir is not.
        write(dir.path() + "/Main.qml", "import QtQuick\nItem { property var a: pinhole.state.x; property var b: pinhole.cache [\"y\"]; property string c: pinhole.cacheDir }\n");
        QCOMPARE(Plugins::apiUses(dir.path()), (QMap<int, QStringList>{{3, {"pinhole.state", "pinhole.cache"}}}));
        // A thumbnail's file: API 4's; its URL not.
        write(dir.path() + "/Main.qml", "import QtQuick\nItem { property string a: \"image://file/\" + encodeURIComponent(p); property string b: item.thumbPath; property url c: item.thumbnail; property string d: \"image://photo/\" }\n");
        QCOMPARE(Plugins::apiUses(dir.path()), (QMap<int, QStringList>{{4, {"a photo's thumbPath"}}}));
        // A layer over the viewer's photo, an action's order, hint and
        // complaint, and fetch's options: API 3's.
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    viewerLayer: Item { property int photo; property bool shown }
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()), (QMap<int, QStringList>{{3, {"viewerLayer"}}}));
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    actions: [ PluginAction { text: "a"; order: -10 }, PluginAction { text: "b"; onTriggered: { const p = { order: 1 } } } ]
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()), (QMap<int, QStringList>{{3, {"an action's order, hint, busyDelay, textColour or complain"}}}));
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    actions: [ PluginAction { id: a; text: "a"; onTriggered: { const p = { hint: 1 }; a.complain("no") } } ]
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()), (QMap<int, QStringList>{{3, {"an action's order, hint, busyDelay, textColour or complain"}}}));
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    Component.onCompleted: pinhole.fetch(0, "live", { stream: true }).then(url => {}, error => {})
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()), (QMap<int, QStringList>{{2, {"promises"}}, {3, {"fetch's options"}}}));
        // A favourite set, the library's revision, an action's words in a
        // colour (not a property of its own of another name): API 3's.
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    readonly property bool b: (pinhole.library.revision, true)
    actions: [ PluginAction { textColour: "accent"; onTriggered: pinhole.setFavorite([0], true) },
               PluginAction { readonly property string colour: "x"; text: colour } ]
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()),
                 (QMap<int, QStringList>{{3, {"an action's order, hint, busyDelay, textColour or complain", "pinhole.setFavorite",
                                              "library.revision"}}}));
        QVERIFY(Plugins::apiUses(builtIn + "/favourites").value(3).contains("pinhole.setFavorite"));
        QVERIFY(Plugins::apiUses(builtIn + "/series").value(3).contains("pinhole.state"));
        QVERIFY(Plugins::apiUses(builtIn + "/screenshots").value(3).contains("pinhole.cache"));
        QVERIFY(Plugins::apiUses(builtIn + "/screenshots").value(2).contains("hidden"));
        QVERIFY(Plugins::apiUses(builtIn + "/send").value(2).contains("run's output"));
        QVERIFY(Plugins::apiUses(builtIn + "/colour").value(2).contains("pinhole.coveredBy"));
        QVERIFY(Plugins::apiUses(builtIn + "/colour").value(2).contains("pinhole.dismissed"));
        QVERIFY(Plugins::apiUses(builtIn + "/colour").value(2).contains("avgColor"));
        QVERIFY(Plugins::apiUses(builtIn + "/frame").value(2).contains("pinhole.keepAwake"));
        QVERIFY(Plugins::apiUses(builtIn + "/background").value(3).contains("fetch's loop, bounce, long_exposure or jpeg"));
        QVERIFY(Plugins::apiUses(builtIn + "/send").value(3).contains("fetch's loop, bounce, long_exposure or jpeg"));
        QVERIFY(Plugins::apiUses(builtIn + "/live").value(3).contains("a photo's effect"));
    }

    // Every one of API 3's, as docs/plugins.md lists them, in a plugin
    // that asks for API 2: each found, and said by `pinhole plugin check`.
    void everythingOfApi3IsFoundInAPluginOfApi2() {
        QTemporaryDir dir;
        write(dir.path() + "/manifest.json", R"({ "id": "me.all", "api": 2, "entryPoints": { "main": "Main.qml" },
            "sources": [{ "kind": "me.all", "provider": "provider" }] })");
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    id: plugin
    readonly property var ways: ({ bounce: { kind: "bounce" } })
    readonly property string shownAs: pinhole.library.item(0).effect
    readonly property bool now: (pinhole.library.revision, true)
    marks: [ PluginMark { name: "x"; field: "live" } ]
    viewerLayer: Item { property int photo; property bool shown }
    actions: [ PluginAction { id: a; text: "a"; order: -1; hint: "h"; busyDelay: 300; textColour: "red"
                              onTriggered: a.complain("no") } ]
    function f() {
        pinhole.state.x = pinhole.cache.y
        pinhole.setFavorite([0], true)
        pinhole.fetch(0, "long_exposure", { stream: true })
        return pinhole.library.field("effect")
    }
}
)");
        const QStringList all = {"marks", "pinhole.state", "pinhole.cache", "viewerLayer",
                                 "an action's order, hint, busyDelay, textColour or complain", "fetch's options",
                                 "fetch's loop, bounce, long_exposure or jpeg", "a photo's effect", "pinhole.setFavorite",
                                 "library.revision", "sources in the manifest"};
        QCOMPARE(Plugins::apiUses(dir.path()).value(3), all);
        // Each kind alone, in the call or named as a kind; a photo's effect
        // alone, by item or by field.
        for (const QString &kind : {"loop", "bounce", "long_exposure", "jpeg"}) {
            write(dir.path() + "/Main.qml", QStringLiteral("import QtQuick\nItem { function f() { return pinhole.fetch(0, '%1') } }\n").arg(kind).toUtf8());
            QCOMPARE(Plugins::apiUses(dir.path()).value(3), QStringList({"fetch's loop, bounce, long_exposure or jpeg", "sources in the manifest"}));
            write(dir.path() + "/Main.qml", QStringLiteral("import QtQuick\nItem { property string kind: \"%1\" }\n").arg(kind).toUtf8());
            QCOMPARE(Plugins::apiUses(dir.path()).value(3), QStringList({"fetch's loop, bounce, long_exposure or jpeg", "sources in the manifest"}));
        }
        write(dir.path() + "/manifest.json", R"({ "id": "me.all", "api": 2 })");
        for (const char *use : {"pinhole.library.item(i).effect", "it.effect", "pinhole.library.field('effect')"}) {
            write(dir.path() + "/Main.qml", QStringLiteral("import QtQuick\nItem { function f(i, it) { return %1 } }\n").arg(QString::fromLatin1(use)).toUtf8());
            QCOMPARE(Plugins::apiUses(dir.path()).value(3), QStringList({"a photo's effect"}));
        }
        // Not: what API 2 fetched, a layer's effect, a kind's words in a
        // sentence or a comment, another field.
        write(dir.path() + "/Main.qml", R"(import QtQuick
// pinhole.fetch(0, "jpeg"), kind: "loop", item.effect
Item {
    property string said: 'kind: "loop" and a jpeg'
    layer.enabled: true
    layer.effect: Item {}
    function f(i) { pinhole.fetch(i, "medium"); pinhole.fetch(i, "live"); return pinhole.library.field("live") + item.layer.effect }
}
)");
        QCOMPARE(Plugins::apiUses(dir.path()).value(3), QStringList());
    }

    // A plugin's file named as another's, or as one of Pinhole's own, is
    // warned of; Pinhole's own plugins' are each their own.
    void aPluginsFileNamedAsAnothersIsWarnedOf() {
        QTemporaryDir dir;
        QDir().mkpath(dir.path() + "/mine");
        QDir().mkpath(dir.path() + "/theirs/parts");
        write(dir.path() + "/mine/Main.qml", "import QtQuick\nItem {}\n");
        write(dir.path() + "/mine/Studio.qml", "import QtQuick\nItem {}\n");
        write(dir.path() + "/mine/Toggle.qml", "import QtQuick\nItem {}\n");
        write(dir.path() + "/mine/Own.js", "function f() {}\n");
        write(dir.path() + "/theirs/Main.qml", "import QtQuick\nItem {}\n");
        write(dir.path() + "/theirs/parts/Studio.qml", "import QtQuick\nItem {}\n");
        QCOMPARE(Plugins::nameClashes(dir.path() + "/mine", {{"me.mine", dir.path() + "/mine"}, {"me.theirs", dir.path() + "/theirs"}}),
                 QStringList({"Studio.qml is named as me.theirs's Studio.qml", "Toggle.qml is named as Pinhole's own Toggle.qml"}));
        const QString builtIn = QStringLiteral(PINHOLE_I18N "/../../../plugins");
        QList<std::pair<QString, QString>> all;
        for (const QFileInfo &plugin : QDir(builtIn).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            all.append({plugin.fileName(), plugin.filePath()});
        for (const auto &[id, folder] : all) {
            const QStringList clashes = Plugins::nameClashes(folder, all);
            QVERIFY2(clashes.isEmpty(), qPrintable(id + ": " + clashes.join("; ")));
        }
    }

    void theLibraryIsOldestFirstAndAMissingOneIsReported() {
        QTemporaryDir dir;
        Library missing;
        QString error;
        QVERIFY(!missing.open(dir.path(), &error));
        QVERIFY(error.contains("No library"));

        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "make");
            db.setDatabaseName(dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE assets (id TEXT, thumb_path TEXT, avg_color INTEGER, thumb_w INTEGER, thumb_h INTEGER,"
                           " duration REAL, asset_date INTEGER, is_video INTEGER, is_live INTEGER, favorite INTEGER, filename TEXT)"));
            QVERIFY(q.exec("INSERT INTO assets VALUES ('b', 'thumbs/b.jpg', 1, 480, 360, 0, 2000, 0, 0, 1, 'B.HEIC'),"
                           " ('a', 'thumbs/a.jpg', 2, 360, 480, 0, 1000, 1, 0, 0, 'A.MOV')"));
            db.close();
        }
        QSqlDatabase::removeDatabase("make");
        Library library;
        QVERIFY(library.open(dir.path(), &error));
        QCOMPARE(library.count(), 2);
        QCOMPARE(library.at(0).id, QByteArray("a"));
        QVERIFY(library.at(0).video);
        QCOMPARE(library.at(1).filename, QString("B.HEIC"));
        QVERIFY(library.at(1).favorite);
        QVERIFY(library.at(1).thumbPath.endsWith("/thumbs/b.jpg"));
        // A library from before effects has none; once the service has
        // added them, a Live Photo has the one set on the iPhone.
        QCOMPARE(library.at(1).effect, LiveEffect::None);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "make");
            db.setDatabaseName(dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE assets ADD COLUMN live_effect TEXT"));
            QVERIFY(q.exec("UPDATE assets SET is_live = 1, live_effect = 'bounce' WHERE id = 'b'"));
            db.close();
        }
        QSqlDatabase::removeDatabase("make");
        Library effects;
        QVERIFY(effects.open(dir.path(), &error));
        QCOMPARE(effects.at(1).effect, LiveEffect::Bounce);
        QCOMPARE(effects.at(0).effect, LiveEffect::None);
    }

    // A copy, export or drag waits (for downloads, the folder picker, the
    // drop); a new photo arriving meanwhile moves every position along. What
    // it hands out must still be the photos that were chosen.
    // A viewer 1000 x 800 showing a 4:3 photo fitted to it, with photos on
    // both sides unless said otherwise.
    static ViewerMotion viewerShowing(bool before = true, bool after = true) {
        ViewerMotion m;
        m.setViewport(1000, 800);
        m.setPhoto(1000, 750, true);
        m.setNeighbours(before, after);
        return m;
    }

    // Fingers moving `perEvent` px sideways every 8 ms, `events` times; then
    // lifted `pause` ms after the last.
    static int swipe(ViewerMotion &m, double perEvent, int events, double pause = 8) {
        double t = 1000;
        m.begin(t);
        for (int i = 0; i < events; ++i)
            m.move(t += 8, perEvent, 0);
        return m.end(t + pause);
    }

    static void settle(ViewerMotion &m) {
        for (int i = 0; i < 600 && m.animating(); ++i)
            m.advance(1.0 / 120);
    }

    void aShortSlowSwipeSpringsBack() {
        ViewerMotion m = viewerShowing();
        QCOMPARE(swipe(m, -2, 20, 200), 0); // 80 px of 1024, then a pause
        QVERIFY(m.offset() < 0);
        settle(m);
        QCOMPARE(m.offset(), 0.0);
    }

    void aLongSwipeOrAFlickLandsOnTheNext() {
        ViewerMotion m = viewerShowing();
        QCOMPARE(swipe(m, -4, 50, 200), 1); // 400 px: past three tenths
        ViewerMotion flick = viewerShowing();
        QCOMPARE(swipe(flick, -6, 8), 1); // short but fast
        // The next photo slides in from the right and settles, never
        // passing its place.
        m.stepped(1);
        QVERIFY(m.offset() > 0);
        double lowest = m.offset();
        for (int i = 0; i < 600 && m.animating(); ++i) {
            m.advance(1.0 / 120);
            lowest = std::min(lowest, m.offset());
        }
        QCOMPARE(m.offset(), 0.0);
        QVERIFY(lowest >= 0);
    }

    // In Photos, swiping again while the last photo is still sliding in
    // pages on: the second swipe is judged by its own direction.
    void aSecondSwipeWhileSlidingPagesOn() {
        ViewerMotion m = viewerShowing();
        QCOMPARE(swipe(m, 6, 10), -1);
        m.stepped(-1); // the previous photo now slides in from the left
        m.advance(1.0 / 120);
        QVERIFY(m.offset() < -m.pageWidth() / 2);
        QCOMPARE(swipe(m, 6, 10), -1);
        QCOMPARE(swipe(m, 0.5, 4, 200), 0); // a nudge, not a page
    }

    // A tap five photos along the carousel: the photos slide in from 1.5
    // pages out, the carousel travels all five, and both land together.
    void aLongStepSlidesThePhotosAndTheWholeCarousel() {
        ViewerMotion m = viewerShowing();
        m.stepped(5);
        const double page = m.pageWidth();
        QCOMPARE(m.offset(), 1.5 * page);
        QCOMPARE(m.stripOffset(), 5 * page);
        for (int i = 0; i < 12; ++i)
            m.advance(1.0 / 120);
        QVERIFY(m.offset() > 0 && m.offset() < 1.5 * page);
        QVERIFY(std::abs(m.stripOffset() / m.offset() - 5 / 1.5) < 1e-6); // in step
        settle(m);
        QCOMPARE(m.offset(), 0.0);
        QCOMPARE(m.stripOffset(), 0.0);
    }

    // A far tap: the photos slide one page, as between neighbours; the
    // carousel travels all five, and both land together.
    void aFarTapSlidesOnePageAndTheWholeCarousel() {
        ViewerMotion m = viewerShowing();
        m.stepped(5, 1);
        const double page = m.pageWidth();
        QCOMPARE(m.offset(), page);
        QCOMPARE(m.stripOffset(), 5 * page);
        for (int i = 0; i < 12; ++i)
            m.advance(1.0 / 120);
        QVERIFY(std::abs(m.stripOffset() / m.offset() - 5.0) < 1e-6);
        settle(m);
        QCOMPARE(m.offset(), 0.0);
        QCOMPARE(m.stripOffset(), 0.0);
    }

    // A key: the photo is there at once and the carousel glides one photo
    // after it, never past; held (40 a second), the carousel scrolls a
    // photo or two behind and lands when it is let go.
    void aKeyChangesPhotoAtOnceAndTheCarouselGlidesAfter() {
        ViewerMotion m = viewerShowing();
        const double page = m.pageWidth();
        m.jumped(1);
        QCOMPARE(m.offset(), 0.0);
        QCOMPARE(m.stripOffset(), page);
        double lowest = page;
        int frames = 0;
        for (; frames < 600 && m.animating(); ++frames) {
            m.advance(1.0 / 120);
            lowest = std::min(lowest, m.stripOffset());
        }
        QCOMPARE(m.stripOffset(), 0.0);
        QVERIFY(lowest >= 0);
        QVERIFY(frames > 12 && frames < 72); // seen to glide, gone in under 0.6 s

        ViewerMotion held = viewerShowing();
        double most = 0;
        for (int frame = 0; frame < 240; ++frame) { // 2 s at 120 fps
            if (frame % 3 == 0)
                held.jumped(1);
            held.advance(1.0 / 120);
            if (frame > 60)
                most = std::max(most, held.stripOffset() / page);
        }
        QVERIFY(most > 0.5 && most < 2.5);
        settle(held);
        QCOMPARE(held.stripOffset(), 0.0);
    }

    // A held key, timed here and not by the system's repeat: the press
    // moves on one photo, and a moment later the photos move on by
    // themselves, one every quarter second, the carousel gliding along
    // without a jump; let go, it lands on the photo shown without passing
    // it. With Shift they go slowly at first and quicker the longer it is
    // held.
    void aHeldKeyStartsSlowlyAndGathersSpeed() {
        // Where it ended, -1 if it moved on after being let go or did not
        // come to rest.
        auto holdFor = [](bool fast, double seconds, QList<double> *times, double *jump, double *past) -> int {
            ViewerMotion m = viewerShowing();
            const double page = m.pageWidth();
            const double dt = 1.0 / 120;
            int index = 1;
            m.jumped(1);
            m.hold(1, fast);
            double last = index - m.stripOffset() / page;
            for (double t = 0; t < seconds; t += dt) {
                m.advance(dt);
                if (const int d = m.takeSteps()) {
                    index += d;
                    if (times)
                        times->append(t);
                }
                const double at = index - m.stripOffset() / page;
                if (jump)
                    *jump = std::max(*jump, std::abs(at - last));
                last = at;
            }
            m.release();
            for (int i = 0; i < 600 && m.animating(); ++i) {
                m.advance(dt);
                if (m.takeSteps())
                    return -1;
                if (past)
                    *past = std::max(*past, -m.stripOffset() / page);
            }
            return m.stripOffset() == 0 ? index : -1;
        };
        QList<double> times;
        double jump = 0, past = 0;
        const int plain = holdFor(false, 3, &times, &jump, &past);
        QVERIFY(plain > 0);
        QVERIFY(times.size() >= 4);
        QVERIFY(times[0] >= 0.2); // nothing before a moment has passed
        for (int i = 1; i < times.size(); ++i) // then a photo every quarter second
            QVERIFY(std::abs(times[i] - times[i - 1] - 0.25) < 0.03);
        QVERIFY(jump < 0.5); // the carousel glides, never jumps
        QVERIFY(past <= 0);
        QList<double> fastTimes;
        jump = 0;
        const int fast = holdFor(true, 3, &fastTimes, &jump, nullptr);
        QVERIFY(fast > plain * 1.5);
        QVERIFY(fastTimes[1] - fastTimes[0] > 0.1); // slow at first
        QVERIFY(fastTimes.last() - fastTimes[fastTimes.size() - 2] < 0.08); // quick later
        QVERIFY(jump < 0.5);
        // Let go before the moment: just the press's one photo.
        QCOMPARE(holdFor(false, 0.2, nullptr, nullptr, nullptr), 1);

        // Shift let go at full speed: the photos slow down at once, not
        // over seconds, and keep going at the plain speed.
        ViewerMotion brake = viewerShowing();
        brake.jumped(1);
        brake.hold(1, true);
        for (int i = 0; i < 360; ++i) {
            brake.advance(1.0 / 120);
            brake.takeSteps();
        }
        brake.setHoldFast(false);
        int later = 0;
        for (int i = 0; i < 96; ++i) {
            brake.advance(1.0 / 120);
            const int d = brake.takeSteps();
            if (i >= 36)
                later += d;
        }
        QVERIFY(later <= 3); // 0.3–0.8 s after: about 4 a second, not 60

        // At the end of the library it stops there.
        ViewerMotion end = viewerShowing(true, false);
        end.jumped(1);
        end.hold(1, true);
        for (int i = 0; i < 240; ++i) {
            end.advance(1.0 / 120);
            QCOMPARE(end.takeSteps(), 0);
        }
        QCOMPARE(end.stripOffset(), 0.0);
    }

    // Fingers moving `perEvent` px up or down every 8 ms, `events` times,
    // lifted `pause` ms after the last.
    static void swipeDown(ViewerMotion &m, double perEvent, int events, double pause = 8) {
        double t = 1000;
        m.begin(t);
        for (int i = 0; i < events; ++i)
            m.move(t += 8, 0, perEvent);
        QCOMPARE(m.end(t + pause), 0);
    }

    void aSmallDragDownSpringsBack() {
        ViewerMotion m = viewerShowing();
        swipeDown(m, 2, 10, 200); // 32 px, then still
        QVERIFY(m.dismiss() > 0);
        QVERIFY(!m.takeClose());
        settle(m);
        QCOMPARE(m.dismiss(), 0.0);
    }

    void aLongDragOrAFlickUpOrDownCloses() {
        ViewerMotion far = viewerShowing();
        swipeDown(far, 3, 40, 200); // 192 px of 800, then still
        QVERIFY(far.takeClose());
        ViewerMotion flick = viewerShowing();
        swipeDown(flick, -5, 6); // short, fast, upward
        QVERIFY(flick.takeClose());
    }

    void aFlickBackTowardTheMiddleDoesNotClose() {
        ViewerMotion m = viewerShowing();
        double t = 1000;
        m.begin(t);
        for (int i = 0; i < 10; ++i)
            m.move(t += 8, 0, 4); // down 64 px
        for (int i = 0; i < 4; ++i)
            m.move(t += 8, 0, -5); // and quickly back up
        m.end(t += 8);
        QVERIFY(!m.takeClose());
    }

    void aMouseDragFollowsThePointerAndClosesAsASwipeDoes() {
        // Grabbed with the mouse, the photo moves as far as the pointer.
        ViewerMotion small = viewerShowing();
        double t = 1000;
        small.begin(t, true);
        for (int i = 0; i < 10; ++i)
            small.move(t += 8, 0, 6); // 60 px down, then held still
        QCOMPARE(small.dismiss(), 60.0);
        QCOMPARE(small.end(t + 200), 0);
        QVERIFY(!small.takeClose());
        settle(small);
        QCOMPARE(small.dismiss(), 0.0);

        ViewerMotion far = viewerShowing();
        t = 1000;
        far.begin(t, true);
        for (int i = 0; i < 30; ++i)
            far.move(t += 8, 0, 5); // 150 px of 800
        far.end(t + 200);
        QVERIFY(far.takeClose());

        // Sideways it pages, past three tenths of the way.
        ViewerMotion page = viewerShowing();
        t = 1000;
        page.begin(t, true);
        for (int i = 0; i < 40; ++i)
            page.move(t += 8, -10, 0); // 400 px of 1024
        QCOMPARE(page.end(t + 200), 1);
    }

    void aZoomedPhotoPansUpAndDownInstead() {
        ViewerMotion m = viewerShowing();
        m.zoomBy(3, 500, 400);
        settle(m);
        swipeDown(m, 3, 40, 200);
        QCOMPARE(m.dismiss(), 0.0);
        QVERIFY(!m.takeClose());
        QVERIFY(m.panY() > 50);
    }

    void theLastPhotoStretchesInsteadOfPaging() {
        ViewerMotion m = viewerShowing(true, false);
        QCOMPARE(swipe(m, -10, 60), 0); // 1200 px of finger movement
        QVERIFY(m.offset() < 0 && m.offset() > -m.pageWidth() / 2);
    }

    void aPinchKeepsThePointBetweenTheFingers() {
        ViewerMotion m = viewerShowing();
        // The point under (700, 300), as a share of the photo, before and after.
        auto share = [&m](double x, double y) {
            const double w = 1000 * m.zoom(), h = 750 * m.zoom();
            const double left = 500 + m.panX() - w / 2, top = 400 + m.panY() - h / 2;
            return QPointF((x - left) / w, (y - top) / h);
        };
        const QPointF before = share(700, 300);
        m.pinchBegin(700, 300);
        m.pinch(2.5, 700, 300);
        const QPointF after = share(700, 300);
        QVERIFY(std::abs(before.x() - after.x()) < 1e-9 && std::abs(before.y() - after.y()) < 1e-9);
        QVERIFY(!m.pinchEnd());
        QCOMPARE(m.zoom(), 2.5);
        QVERIFY(m.zoomed());
    }

    void pinchingInSpringsBackOrCloses() {
        ViewerMotion m = viewerShowing();
        m.pinchBegin(500, 400);
        m.pinch(0.85, 500, 400);
        QVERIFY(!m.pinchEnd());
        settle(m);
        QCOMPARE(m.zoom(), 1.0);
        m.pinchBegin(500, 400);
        m.pinch(0.5, 500, 400);
        QVERIFY(m.pinchEnd()); // close
    }

    void aZoomedPhotoPansWithinItsEdgesAndDoesNotPage() {
        ViewerMotion m = viewerShowing();
        m.zoomBy(2, 500, 400);
        settle(m);
        QCOMPARE(m.zoom(), 2.0);
        QCOMPARE(swipe(m, -50, 40), 0); // would page if not zoomed
        QCOMPARE(m.offset(), 0.0);
        settle(m); // pulled past the edge: springs back to it
        // 2000 x 1500 in 1000 x 800: 500 px of room each way across.
        QCOMPARE(m.panX(), -500.0 * 1);
        QVERIFY(std::abs(m.panY()) <= 350);
    }

    // Zoomed in, a flick keeps the photo moving after the fingers lift,
    // slowing down as the grid's scrolling does; a resting finger stops it.
    void aZoomedFlickCoastsAndARestingFingerStopsIt() {
        ViewerMotion m = viewerShowing();
        m.zoomBy(4, 500, 400); // 4000 x 3000: 1500 px of room each way across
        settle(m);
        double t = 1000;
        m.begin(t);
        for (int i = 0; i < 10; ++i)
            m.move(t += 8, -6, 0);
        m.end(t += 8);
        const double lifted = m.panX();
        for (int i = 0; i < 12; ++i)
            m.advance(1.0 / 120);
        QVERIFY(m.panX() < lifted - 20); // still going the same way
        QVERIFY(m.animating());
        m.touch(t + 100);
        const double stopped = m.panX();
        for (int i = 0; i < 60; ++i)
            m.advance(1.0 / 120);
        QCOMPARE(m.panX(), stopped);
    }

    // The same flick coasts a zoomed photo a short way, and is over in
    // about half a second; the grid coasts several times as far.
    void aZoomedPhotoCoastsShortCompared() {
        ViewerMotion m = viewerShowing();
        m.zoomBy(8, 500, 400); // plenty of room
        settle(m);
        double t = 1000;
        m.begin(t);
        for (int i = 0; i < 10; ++i)
            m.move(t += 8, -6, 0);
        m.end(t += 8);
        const double lifted = m.panX();
        int frames = 0;
        for (; frames < 600 && m.animating(); ++frames)
            m.advance(1.0 / 120);
        const double photoCoast = lifted - m.panX();

        ScrollPhysics grid;
        grid.setBounds(-1e9, 1e9, 800);
        double g = 1000;
        grid.begin(g);
        for (int i = 0; i < 10; ++i)
            grid.move(g += 8, -6 * 1.6); // the pan's extra gain, to compare like with like
        grid.end(g += 8);
        const double gridLifted = grid.position();
        while (grid.step(1.0 / 120)) {}
        const double gridCoast = gridLifted - grid.position();

        QVERIFY2(frames < 80, qPrintable(QString::number(frames))); // under ~0.65 s
        QVERIFY2(photoCoast > 20 && photoCoast < gridCoast / 3,
                 qPrintable(QStringLiteral("%1 vs %2").arg(photoCoast).arg(gridCoast)));
    }

    void pullingAZoomedPhotoPastItsEdgeStretchesAndSpringsBack() {
        ViewerMotion m = viewerShowing();
        m.zoomBy(2, 500, 400); // 500 px of room each way across
        settle(m);
        double t = 1000;
        m.begin(t);
        for (int i = 0; i < 120; ++i)
            m.move(t += 16, -4, 0); // slow: no throw
        // Pulled well over a thousand pixels past the edge, it gives a
        // fraction of that: under 40% of the window.
        QVERIFY2(m.panX() < -500 && m.panX() > -900, qPrintable(QString::number(m.panX())));
        m.end(t + 200);
        settle(m);
        QCOMPARE(m.panX(), -500.0);
    }

    // Pulled past the edge and let go, then zoomed by keys before it had
    // sprung back: the zoom decides where it goes, not the old edge.
    void aZoomTakesOverFromASpringBack() {
        ViewerMotion m = viewerShowing();
        m.zoomBy(2, 500, 400);
        settle(m);
        double t = 1000;
        m.begin(t);
        for (int i = 0; i < 120; ++i)
            m.move(t += 16, -4, 0); // past the left edge
        m.end(t + 200);
        m.advance(1.0 / 120); // springing back
        m.resetZoom();
        m.zoomBy(3, 500, 400);
        settle(m);
        QCOMPARE(m.zoom(), 3.0);
        QCOMPARE(m.panX(), 0.0);
    }

    void aReloadSaysWhereEachPhotoWentAndReadsAgainForAChangeMeanwhile() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}});
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        LibraryReloader reloader(&library, &loader, dir.path());
        std::vector<std::vector<int>> moves;
        std::vector<bool> newest;
        int swaps = 0;
        connect(&reloader, &LibraryReloader::aboutToSwap, this, [&] { ++swaps; });
        connect(&reloader, &LibraryReloader::reloaded, this,
                [&](const std::vector<int> &moved, const std::vector<int> &, bool toNewest) {
                    moves.push_back(moved);
                    newest.push_back(toNewest);
                });

        // b is gone and one came first: a to 1, c to 2.
        QFile::remove(dir.path() + "/library.sqlite");
        makeLibrary(dir.path(), {{"new", 0}, {"a", 1}, {"c", 3}});
        const quint64 generation = loader.generation();
        reloader.showNewestNext();
        reloader.reload();
        // A change announced while that is read: one more read after it.
        reloader.reload();
        QTRY_COMPARE(moves.size(), size_t(2));
        QCOMPARE(swaps, 2);
        QCOMPARE(moves[0], (std::vector<int>{1, -1, 2}));
        QCOMPARE(moves[1], (std::vector<int>{0, 1, 2}));
        QCOMPARE(newest, (std::vector<bool>{true, false}));
        QCOMPARE(library.count(), 3);
        QCOMPARE(library.at(0).id, QByteArray("new"));
        QCOMPARE(loader.generation(), generation + 2);
        QTest::qWait(50);
        QCOMPARE(moves.size(), size_t(2));
    }

    void aPhotoIsReadForWhatItIsWhateverItsNameSays() {
        // iCloud calls a loop's JPEG a HEIC, and the cache names it so.
        QTemporaryDir dir;
        QImage picture(64, 48, QImage::Format_RGB32);
        picture.fill(Qt::red);
        QVERIFY(picture.save(dir.path() + "/loop.heic", "JPEG"));
        const QImage read = decodePhoto(dir.path() + "/loop.heic", QSize());
        QCOMPARE(read.size(), QSize(64, 48));
    }

    void aPhotoIsDecodedAtTheSizeAskedAndNotAtAllOnceDropped() {
        QTemporaryDir dir;
        QImage picture(400, 300, QImage::Format_RGB32);
        picture.fill(Qt::red);
        QVERIFY(picture.save(dir.path() + "/a.jpg", "JPEG"));
        QCOMPARE(decodePhoto(dir.path() + "/a.jpg", QSize(200, 200)).size(), QSize(200, 150));
        std::atomic<bool> dropped{true};
        QVERIFY(decodePhoto(dir.path() + "/a.jpg", QSize(200, 200), &dropped).isNull());
    }

    // Colour (colour.h): a picture of flat patches, each 16 px square, in
    // the codes given, tagged with `icc` if any, as a JPEG of quality 100.
    static QImage patches(const std::vector<QRgb> &codes) {
        QImage picture(16 * int(codes.size()), 16, QImage::Format_RGB32);
        for (size_t k = 0; k < codes.size(); ++k)
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x)
                    picture.setPixel(16 * int(k) + x, y, codes[k]);
        return picture;
    }
    static QByteArray saveJpeg(const QString &path, QImage picture, const QByteArray &icc) {
        if (!icc.isEmpty())
            picture.setColorSpace(QColorSpace::fromIccProfile(icc));
        if (!picture.save(path, "JPEG", 100))
            return {};
        return QFile::encodeName(path);
    }
    // The middle of patch k, as decoded.
    static QRgb patch(const QImage &image, int k) { return image.pixel(16 * k + 8, 8); }
    static bool near(QRgb a, QRgb b, int within) {
        return std::abs(qRed(a) - qRed(b)) <= within && std::abs(qGreen(a) - qGreen(b)) <= within && std::abs(qBlue(a) - qBlue(b)) <= within;
    }
    // What lcms2 makes of codes in `icc`, relative colorimetric to sRGB: the
    // reference, by a path of lcms2's own.
    static QRgb lcms2Srgb(const QByteArray &icc, QRgb code) {
        cmsHPROFILE from = cmsOpenProfileFromMem(icc.constData(), cmsUInt32Number(icc.size())), to = cmsCreate_sRGBProfile();
        cmsHTRANSFORM t = cmsCreateTransform(from, TYPE_RGB_8, to, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
        const uchar in[3] = {uchar(qRed(code)), uchar(qGreen(code)), uchar(qBlue(code))};
        uchar out[3];
        cmsDoTransform(t, in, out, 1);
        cmsDeleteTransform(t);
        cmsCloseProfile(from);
        cmsCloseProfile(to);
        return qRgb(out[0], out[1], out[2]);
    }
    static QByteArray testData(const QString &name) {
        QFile file(QStringLiteral(PINHOLE_TEST_DATA "/") + name);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    void aDisplayP3PhotoIsShownInSrgbEverywhereItIsDecoded() {
        QTemporaryDir dir;
        // Pure green, a deep red, a mid grey, as an iPhone would write them
        // (Apple's own Display P3 profile, from a real thumbnail).
        const QByteArray p3 = testData("display-p3.icc");
        QVERIFY(!p3.isEmpty());
        const std::vector<QRgb> codes = {qRgb(0, 255, 0), qRgb(200, 30, 40), qRgb(128, 128, 128)};
        const QByteArray file = saveJpeg(dir.path() + "/p3.jpg", patches(codes), p3);
        QVERIFY(!file.isEmpty());
        // The viewer's medium and full size (decodePhoto), the grid's
        // thumbnails (the loader) and every Image of a thumbnail
        // (image://file): each in sRGB, as lcms2 has it.
        FileProvider files;
        QSize size;
        const QImage decoded[] = {decodePhoto(QFile::decodeName(file), QSize()), decodeThumbnail(file, 0),
                                  files.requestImage(QString::fromUtf8(QUrl::toPercentEncoding(QFile::decodeName(file))), &size,
                                                     QSize(), QQuickImageProviderOptions())};
        for (const QImage &image : decoded) {
            QCOMPARE(image.size(), QSize(48, 16));
            // P3's green is outside sRGB: sRGB's green, clipped.
            QVERIFY2(near(patch(image, 0), qRgb(0, 255, 0), 1), qPrintable(QColor(patch(image, 0)).name()));
            QVERIFY2(near(patch(image, 1), lcms2Srgb(p3, qRgb(200, 30, 40)), 2), qPrintable(QColor(patch(image, 1)).name()));
            // The red is redder in sRGB's codes than P3's: it was shown too
            // dull before.
            QVERIFY(qRed(patch(image, 1)) > 205 && qGreen(patch(image, 1)) < 25);
            QVERIFY(near(patch(image, 2), qRgb(128, 128, 128), 1));
        }
        QCOMPARE(size, QSize(48, 16));
    }

    void anUntaggedPhotoComesBackByteForByte() {
        QTemporaryDir dir;
        const QByteArray file = saveJpeg(dir.path() + "/plain.jpg", patches({qRgb(0, 255, 0), qRgb(200, 30, 40)}), {});
        QImageReader reader(QFile::decodeName(file));
        const QImage plain = reader.read().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(decodePhoto(QFile::decodeName(file), QSize()).convertToFormat(QImage::Format_RGB32), plain);
        // The loader's as turbojpeg alone gives it.
        tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
        QFile in(QFile::decodeName(file));
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray bytes = in.readAll();
        QImage turbo(32, 16, QImage::Format_RGBX8888);
        QVERIFY(tj3DecompressHeader(tj, reinterpret_cast<const uchar *>(bytes.constData()), size_t(bytes.size())) == 0);
        QVERIFY(tj3Decompress8(tj, reinterpret_cast<const uchar *>(bytes.constData()), size_t(bytes.size()), turbo.bits(),
                               int(turbo.bytesPerLine()), TJPF_RGBX) == 0);
        tj3Destroy(tj);
        QCOMPARE(decodeThumbnail(file, 0), turbo);
        // sRGB's own profile, as a camera writes it: nothing done either.
        const QByteArray tagged = saveJpeg(dir.path() + "/srgb.jpg", patches({qRgb(0, 255, 0), qRgb(200, 30, 40)}),
                                           QColorSpace(QColorSpace::SRgb).iccProfile());
        QCOMPARE(patch(decodeThumbnail(tagged, 0), 1), patch(decodeThumbnail(file, 0), 1));
    }

    void everyProfileOfARealLibraryConvertsAsLcms2Does() {
        // A gradient through every hue and level, in each profile Jacek's
        // thumbnails carry that is not sRGB: Display P3, and the "Camera RGB
        // Profile" of older iPhones; and one of tables (A2B0) rather than a
        // matrix, which lcms2 converts.
        QImage picture(256, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 256; ++x)
                picture.setPixelColor(x, y, QColor::fromHsv(x * 359 / 255, 255 - y * 2, 255 - (x * y) % 200));
        QByteArray tables;
        {
            cmsHPROFILE profile = cmsCreateProfilePlaceholder(nullptr);
            cmsSetProfileVersion(profile, 4.3);
            cmsSetDeviceClass(profile, cmsSigInputClass);
            cmsSetColorSpace(profile, cmsSigRgbData);
            cmsSetPCS(profile, cmsSigXYZData);
            cmsWriteTag(profile, cmsSigMediaWhitePointTag, cmsD50_XYZ());
            // P3's primaries (D50), behind sRGB's curve, as a pipeline.
            const double matrix[9] = {0.5151, 0.2920, 0.1571, 0.2412, 0.6922, 0.0666, -0.0011, 0.0419, 0.7841};
            const double parameters[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
            cmsToneCurve *curve = cmsBuildParametricToneCurve(nullptr, 4, parameters);
            cmsToneCurve *curves[3] = {curve, curve, curve};
            // As ICC v4 has an A2B0 of curves, a matrix and curves (none).
            cmsPipeline *pipeline = cmsPipelineAlloc(nullptr, 3, 3);
            cmsPipelineInsertStage(pipeline, cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, curves));
            cmsPipelineInsertStage(pipeline, cmsAT_END, cmsStageAllocMatrix(nullptr, 3, 3, matrix, nullptr));
            cmsPipelineInsertStage(pipeline, cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, nullptr));
            cmsWriteTag(profile, cmsSigAToB0Tag, pipeline);
            cmsUInt32Number size = 0;
            cmsSaveProfileToMem(profile, nullptr, &size);
            tables.resize(size);
            cmsSaveProfileToMem(profile, tables.data(), &size);
            cmsPipelineFree(pipeline);
            cmsFreeToneCurve(curve);
            cmsCloseProfile(profile);
        }
        for (const QByteArray &icc : {testData("display-p3.icc"), testData("camera-rgb.icc"), tables}) {
            QVERIFY(!icc.isEmpty());
            QImage converted = picture.copy();
            toSrgb(converted, icc);
            QVERIFY(converted != picture);
            int worst = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 256; ++x) {
                    const QRgb a = converted.pixel(x, y), b = lcms2Srgb(icc, picture.pixel(x, y));
                    worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
                }
            QVERIFY2(worst <= 1, qPrintable(QStringLiteral("%1 codes apart").arg(worst)));
            // Whole and in parts on every core, the same.
            QImage big = picture.scaled(2048, 640);
            QImage expected = big.copy(0, 0, 2048, 8);
            toSrgb(big, icc);
            toSrgb(expected, icc);
            QCOMPARE(big.copy(0, 0, 2048, 8), expected);
        }
    }

    void aHeicSaysItsColoursByNclxOrByProfile() {
        if (QStandardPaths::findExecutable("heif-enc").isEmpty())
            QSKIP("no heif-enc");
        QTemporaryDir dir;
        const std::vector<QRgb> codes = {qRgb(0, 255, 0), qRgb(200, 30, 40), qRgb(128, 128, 128)};
        const QByteArray p3 = testData("display-p3.icc");
        // Lossless, as RGB: the codes as they were.
        QVERIFY(patches(codes).save(dir.path() + "/plain.png"));
        auto encode = [&](const QString &png, const QString &heic, const QStringList &nclx) {
            QProcess process;
            process.start("heif-enc", QStringList{"-L", "-o", heic} + nclx + QStringList{png});
            return process.waitForFinished(30000) && process.exitCode() == 0;
        };
        // An iPhone's edit says P3 by nclx (primaries 12, transfer BT.709).
        QVERIFY(encode(dir.path() + "/plain.png", dir.path() + "/nclx.heic",
                       {"--colour_primaries", "12", "--transfer_characteristic", "1", "--matrix_coefficients", "0"}));
        // A PNG's iCCP becomes the HEIC's profile.
        QImage tagged = patches(codes);
        tagged.setColorSpace(QColorSpace::fromIccProfile(p3));
        QVERIFY(tagged.save(dir.path() + "/p3.png"));
        QVERIFY(encode(dir.path() + "/p3.png", dir.path() + "/icc.heic", {}));
        // And one that says sRGB.
        QVERIFY(encode(dir.path() + "/plain.png", dir.path() + "/srgb.heic",
                       {"--colour_primaries", "1", "--transfer_characteristic", "13", "--matrix_coefficients", "0"}));
        for (const QString name : {"nclx.heic", "icc.heic"}) {
            const QImage image = decodePhoto(dir.path() + '/' + name, QSize());
            QCOMPARE(image.size(), QSize(48, 16));
            QVERIFY2(near(patch(image, 0), qRgb(0, 255, 0), 1), qPrintable(name + ": " + QColor(patch(image, 0)).name()));
            QVERIFY2(near(patch(image, 1), lcms2Srgb(p3, qRgb(200, 30, 40)), 2), qPrintable(name + ": " + QColor(patch(image, 1)).name()));
            QVERIFY(near(patch(image, 2), qRgb(128, 128, 128), 1));
        }
        const QImage plain = decodePhoto(dir.path() + "/srgb.heic", QSize());
        for (int k = 0; k < 3; ++k)
            QVERIFY(near(patch(plain, k), codes[size_t(k)], 1));
        // Scaled first, then converted: the same colours.
        const QImage small = decodePhoto(dir.path() + "/nclx.heic", QSize(24, 8));
        QCOMPARE(small.size(), QSize(24, 8));
        QVERIFY(near(small.pixel(4, 4), qRgb(0, 255, 0), 2));
    }

    // ---- HDR in SDR (docs/hdr.md, H2) ----

    // Linear BT.2020 light (PQ's in nits, HLG's display light after its
    // OOTF) made SDR by the formulas themselves, in double precision:
    // BT.2390's EETF to SDR white on the largest channel, BT.2020 to sRGB,
    // clipped, encoded.
    static QRgb sdrOfLight(double r, double g, double b, double peak) {
        static const double toSrgb[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};
        const double most = std::max({r, g, b});
        const double f = most > 1e-6 ? eetfToSdr(most, peak) / most : 1 / kSdrWhiteNits;
        int out[3];
        for (int k = 0; k < 3; ++k) {
            double v = std::clamp((toSrgb[k * 3] * r + toSrgb[k * 3 + 1] * g + toSrgb[k * 3 + 2] * b) * f, 0.0, 1.0);
            v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            out[k] = int(std::lround(v * 255));
        }
        return qRgb(out[0], out[1], out[2]);
    }
    // R'G'B' from 0 to 1 in PQ or HLG, as the formulas make it SDR.
    static QRgb sdrOfCodes(double r, double g, double b, bool hlg, double peak) {
        double c[3] = {r, g, b};
        for (double &v : c)
            v = hlg ? hlgSceneLight(v) : pqToNits(v);
        if (hlg) {
            const double ys = 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2];
            const double gain = ys > 0 ? kHlgPeakNits * std::pow(ys, 0.2) : 0;
            for (double &v : c)
                v *= gain;
        }
        return sdrOfLight(c[0], c[1], c[2], hlg ? kHlgPeakNits : peak);
    }

    void theEetfKeepsWhatIsBelowItsKneeAndPutsThePeakAtSdrWhite() {
        // Below the knee, light as it is (1 is SDR white, 203 nits).
        QVERIFY(std::abs(eetfToSdr(20, 1000) - 20 / kSdrWhiteNits) < 1e-6);
        QVERIFY(std::abs(eetfToSdr(1000, 1000) - 1) < 1e-6);
        QVERIFY(std::abs(eetfToSdr(4000, 1000) - 1) < 1e-6);
        // Rising all the way, and SDR white itself made room for highlights.
        double last = 0;
        for (double nits = 1; nits <= 1000; nits *= 1.1) {
            const double v = eetfToSdr(nits, 1000);
            QVERIFY2(v > last, qPrintable(QString::number(nits)));
            last = v;
        }
        QVERIFY(eetfToSdr(kSdrWhiteNits, 1000) < 1 && eetfToSdr(kSdrWhiteNits, 1000) > 0.7);
        // A picture no brighter than SDR: as it is.
        QVERIFY(std::abs(eetfToSdr(100, 150) - 100 / kSdrWhiteNits) < 1e-9);
        // PQ there and back.
        for (double nits : {0.01, 1.0, 100.0, 203.0, 1000.0, 10000.0})
            QVERIFY(std::abs(pqToNits(nitsToPq(nits)) - nits) < nits * 1e-6);
    }

    void aPqOrHlgStillIsToneMappedAsTheFormulasSay() {
        if (QStandardPaths::findExecutable("heif-enc").isEmpty())
            QSKIP("no heif-enc");
        QTemporaryDir dir;
        // A 16-bit PNG: a ramp over every code, and four patches.
        QImage png(256, 64, QImage::Format_RGBX64);
        const double patchesRgb[4][3] = {{0.8, 0.1, 0.1}, {0.1, 0.7, 0.2}, {0.15, 0.2, 0.85}, {0.6, 0.6, 0.6}};
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 256; ++x) {
                if (y < 32) {
                    const quint16 v = quint16(x * 257);
                    png.setPixelColor(x, y, QColor::fromRgba64(v, v, v));
                } else {
                    const double *p = patchesRgb[x / 64];
                    png.setPixelColor(x, y, QColor::fromRgbF(float(p[0]), float(p[1]), float(p[2])));
                }
            }
        QVERIFY(png.save(dir.path() + "/still.png"));
        for (const bool hlg : {false, true}) {
            // 10 bits, losslessly as RGB, so the codes are the PNG's; AVIF,
            // as this x265 makes no 10-bit HEIC. Primaries BT.2020.
            const QString file = dir.path() + (hlg ? "/hlg.avif" : "/pq.avif");
            QProcess process;
            process.start("heif-enc", {"-A", "-L", "-b", "10", "--colour_primaries", "9", "--transfer_characteristic",
                                       hlg ? "18" : "16", "-o", file, dir.path() + "/still.png"});
            QVERIFY(process.waitForFinished(60000) && process.exitCode() == 0);
            const QImage image = decodePhoto(file, QSize());
            QCOMPARE(image.size(), png.size());
            // The reference from libheif's own 16-bit decode.
            heif_context *context = heif_context_alloc();
            heif_image_handle *handle = nullptr;
            heif_image *decoded = nullptr;
            QVERIFY(heif_context_read_from_file(context, QFile::encodeName(file).constData(), nullptr).code == heif_error_Ok);
            QVERIFY(heif_context_get_primary_image_handle(context, &handle).code == heif_error_Ok);
            QVERIFY(heif_decode_image(handle, &decoded, heif_colorspace_RGB, heif_chroma_interleaved_RRGGBB_LE, nullptr).code
                    == heif_error_Ok);
            size_t stride = 0;
            const uint8_t *plane = heif_image_get_plane_readonly2(decoded, heif_channel_interleaved, &stride);
            const double top = (1 << heif_image_get_bits_per_pixel_range(decoded, heif_channel_interleaved)) - 1;
            int worst = 0;
            for (int y = 0; y < png.height(); ++y)
                for (int x = 0; x < png.width(); ++x) {
                    const auto *px = reinterpret_cast<const uint16_t *>(plane + stride * size_t(y)) + 3 * x;
                    const QRgb want = sdrOfCodes(px[0] / top, px[1] / top, px[2] / top, hlg, 1000);
                    const QRgb got = image.pixel(x, y);
                    worst = std::max({worst, std::abs(qRed(got) - qRed(want)), std::abs(qGreen(got) - qGreen(want)),
                                      std::abs(qBlue(got) - qBlue(want))});
                }
            heif_image_release(decoded);
            heif_image_handle_release(handle);
            heif_context_free(context);
            QVERIFY2(worst <= 2, qPrintable(QStringLiteral("%1: %2 off").arg(hlg ? "HLG" : "PQ").arg(worst)));
            // Its peak and above (PQ's ramp goes to 10,000 nits) is SDR white.
            QCOMPARE(image.pixel(255, 0), qRgb(255, 255, 255));
            // And Info's: what kind, how many stops.
            const HdrInfo info = readHdr(file);
            QCOMPARE(info.kind(), QString(hlg ? "hlg" : "pq"));
            QVERIFY(std::abs(info.headroom() - std::log2(1000 / kSdrWhiteNits)) < 0.01);
            // Scaled to fit: the same colours.
            const QImage small = decodePhoto(file, QSize(128, 32));
            QCOMPARE(small.size(), QSize(128, 32));
            QVERIFY(near(small.pixel(16, 24), image.pixel(32, 48), 2));
        }
    }

    // A JPEG as an iPhone's medium has its gain map: Apple's MakerNote in
    // its Exif, an MPF index, and a second image with Apple's XMP and ISO's
    // APP2. Made here, with only those: no picture, no place.
    static QByteArray gainMapJpeg(double maker33, double maker48, double xmpHeadroom, double isoStops, bool adobe = false) {
        auto u16 = [](QByteArray &b, quint16 v) { b.append(char(v >> 8)).append(char(v & 255)); };
        auto u32 = [&](QByteArray &b, quint32 v) { u16(b, quint16(v >> 16)); u16(b, quint16(v & 0xffff)); };
        auto segment = [&](quint8 marker, const QByteArray &payload) {
            QByteArray s;
            s.append(char(0xff)).append(char(marker));
            u16(s, quint16(payload.size() + 2));
            return s + payload;
        };
        // Exif: IFD0 → Exif IFD → MakerNote, "Apple iOS" with tags 33 and 48.
        QByteArray note("Apple iOS\0\0\x01MM", 14);
        u16(note, 2);
        const auto entry = [&](QByteArray &b, quint16 tag, quint16 type, quint32 count, quint32 value) {
            u16(b, tag), u16(b, type), u32(b, count), u32(b, value);
        };
        entry(note, 33, 10, 1, 14 + 2 + 24 + 4);
        entry(note, 48, 10, 1, 14 + 2 + 24 + 4 + 8);
        u32(note, 0);
        u32(note, quint32(qint32(std::lround(maker33 * 1e6)))), u32(note, 1000000);
        u32(note, quint32(qint32(std::lround(maker48 * 1e6)))), u32(note, 1000000);
        QByteArray tiff("MM\0*", 4);
        u32(tiff, 8);
        u16(tiff, 1);
        entry(tiff, 0x8769, 4, 1, 26);
        u32(tiff, 0);
        u16(tiff, 1);
        entry(tiff, 0x927c, 7, quint32(note.size()), 44);
        u32(tiff, 0);
        tiff += note;
        // The second image: XMP and ISO's metadata.
        QByteArray second("\xff\xd8", 2);
        QByteArray xmp = QByteArray("http://ns.adobe.com/xap/1.0/", 29);
        if (adobe)
            xmp += QStringLiteral("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF><rdf:Description hdrgm:Version=\"1.0\" "
                                  "hdrgm:HDRCapacityMax=\"%1\"/></rdf:RDF></x:xmpmeta>").arg(isoStops).toUtf8();
        else
            xmp += QStringLiteral("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF><rdf:Description "
                                  "xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">"
                                  "<HDRGainMap:HDRGainMapVersion>131072</HDRGainMap:HDRGainMapVersion>"
                                  "<HDRGainMap:HDRGainMapHeadroom>%1</HDRGainMap:HDRGainMapHeadroom>"
                                  "</rdf:Description></rdf:RDF></x:xmpmeta>").arg(xmpHeadroom, 0, 'f', 6).toUtf8();
        second += segment(0xe1, xmp);
        if (!adobe) {
            QByteArray iso("urn:iso:std:iso:ts:21496:-1", 28);
            u16(iso, 0), u16(iso, 0);
            iso.append(char(0));
            u32(iso, 0), u32(iso, 1);
            u32(iso, quint32(std::lround(isoStops * 1000))), u32(iso, 1000);
            u32(iso, 0), u32(iso, 1);
            u32(iso, quint32(std::lround(isoStops * 1000))), u32(iso, 1000);
            u32(iso, 1), u32(iso, 1);
            u32(iso, 1), u32(iso, 64);
            u32(iso, 1), u32(iso, 64);
            second += segment(0xe2, iso);
        }
        second += QByteArray("\xff\xd9", 2);
        // The primary: its Exif, then the MPF index pointing at the second.
        QByteArray primary("\xff\xd8", 2);
        primary += segment(0xe1, QByteArray("Exif\0\0", 6) + tiff);
        QByteArray mpf("MPF\0", 4);
        mpf += QByteArray("MM\0*", 4);
        u32(mpf, 8);
        u16(mpf, 1);
        entry(mpf, 0xb002, 7, 32, 26);
        u32(mpf, 0);
        const qsizetype mpfAt = primary.size() + 4;
        const qsizetype total = primary.size() + 4 + mpf.size() + 32 + 2;
        u32(mpf, 0x30000), u32(mpf, quint32(total)), u32(mpf, 0), u32(mpf, 0);
        u32(mpf, 0), u32(mpf, quint32(second.size())), u32(mpf, quint32(total - (mpfAt + 4))), u32(mpf, 0);
        primary += segment(0xe2, mpf);
        primary += QByteArray("\xff\xd9", 2);
        return primary + second;
    }

    void aGainMapIsReadFromAJpegsMetadata() {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &bytes) {
            QFile f(dir.path() + '/' + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        };
        // An iPhone's medium (2.79 stops: MakerNote 33 above 1, 48 0.003;
        // XMP 6.917, linear), ISO's and Apple's both.
        write("medium.jpg", gainMapJpeg(1.0, 0.003, std::exp2(2.79), 2.79));
        HdrInfo info = readHdr(dir.path() + "/medium.jpg");
        QVERIFY(info.apple && info.iso);
        QCOMPARE(info.kind(), QString("iso"));
        QVERIFY(std::abs(info.headroom() - 2.79) < 0.01);
        // HDRGainMapHeadroom is linear: its log2 is the MakerNote's stops.
        QVERIFY(std::abs(info.xmpStops - info.makerStops) < 0.1);
        QCOMPARE(info.toMap().value("kind").toString(), QString("iso"));
        QCOMPARE(info.toMap().value("headroom").toDouble(), 2.79);
        // The MakerNote's other branches.
        QVERIFY(std::abs(appleMakerStops(0.5, 0.005) - 1.7) < 1e-9);
        QVERIFY(std::abs(appleMakerStops(0.5, 0.5) - 1.5505) < 1e-9);
        QVERIFY(std::abs(appleMakerStops(1.5, 0.5) - 2.1515) < 1e-9);
        QCOMPARE(appleMakerStops(1.5, 20), 0.0);
        // Adobe's (Ultra HDR) by its XMP.
        write("adobe.jpg", gainMapJpeg(0, 0, 0, 2.3, true));
        info = readHdr(dir.path() + "/adobe.jpg");
        QCOMPARE(info.kind(), QString("iso"));
        QVERIFY(std::abs(info.headroom() - 2.3) < 1e-9);
        // A JPEG with none: SDR, {} for QML.
        QImage plain(8, 8, QImage::Format_RGB32);
        plain.fill(Qt::gray);
        QVERIFY(plain.save(dir.path() + "/plain.jpg"));
        QVERIFY(readHdr(dir.path() + "/plain.jpg").toMap().isEmpty());
        QVERIFY(readHdr(dir.path() + "/none.jpg").kind().isEmpty());
        // Cut short anywhere, it reads what is there and never crashes.
        const QByteArray whole = gainMapJpeg(1.0, 0.003, std::exp2(2.79), 2.79);
        for (qsizetype cut = 0; cut < whole.size(); cut += 7) {
            write("cut.jpg", whole.left(cut));
            readHdr(dir.path() + "/cut.jpg");
        }
    }

    void realGainMapsAgreeInEveryKind() {
        // PINHOLE_HDR_SAMPLE: a folder of real HEICs and JPEGs (a cache's
        // originals and mediums), read only. Skipped without.
        const QString sample = qEnvironmentVariable("PINHOLE_HDR_SAMPLE");
        if (sample.isEmpty())
            QSKIP("no PINHOLE_HDR_SAMPLE");
        int heics = 0, heicBoth = 0, jpegs = 0, jpegHdr = 0, maps = 0, hdr = 0;
        double worst = 0;
        QDirIterator it(sample, {"*.heic", "*.HEIC", "*.jpg", "*.JPG"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const HdrInfo info = readHdr(path);
            const bool heic = path.endsWith(".heic", Qt::CaseInsensitive);
            heics += heic, jpegs += !heic;
            if (heic && info.apple && info.iso)
                ++heicBoth;
            if (!heic && !info.kind().isEmpty())
                ++jpegHdr;
            if (info.xmpStops >= 0 && info.makerStops >= 0)
                worst = std::max(worst, std::abs(info.xmpStops - info.makerStops));
            if (info.iso && info.apple)
                worst = std::max(worst, std::abs(info.isoStops - info.appleStops));
            // Its map's pixels (H4), of the kind it says, upright as the
            // photo: the same shape.
            if (info.kind() != QLatin1String("iso") && info.kind() != QLatin1String("apple"))
                continue;
            const GainMap map = readGainMap(path);
            QSize size;
            if (heic) {
                heif_context *context = heif_context_alloc();
                heif_image_handle *primary = nullptr;
                if (heif_context_read_from_file(context, QFile::encodeName(path).constData(), nullptr).code == heif_error_Ok
                    && heif_context_get_primary_image_handle(context, &primary).code == heif_error_Ok) {
                    size = QSize(heif_image_handle_get_width(primary), heif_image_handle_get_height(primary));
                    heif_image_handle_release(primary);
                }
                heif_context_free(context);
            } else {
                QImageReader reader(path);
                size = reader.size();
                if (reader.transformation() & QImageIOHandler::TransformationRotate90)
                    size.transpose();
            }
            const double shape = map.image.isNull() || size.isEmpty() ? 0 :
                double(map.image.width()) / map.image.height() / (double(size.width()) / size.height());
            if (map.kind == info.kind() && std::abs(shape - 1) < 0.02)
                ++maps;
            else
                qInfo("%s: %s map %s, %dx%d for %dx%d", qPrintable(path), qPrintable(info.kind()), qPrintable(map.kind),
                      map.image.width(), map.image.height(), size.width(), size.height());
            ++hdr;
        }
        qInfo("%d HEICs, %d with Apple's map and ISO's; %d JPEGs, %d HDR; kinds and MakerNote within %.3f EV; %d of %d maps "
              "decoded, of their kind and shape", heics, heicBoth, jpegs, jpegHdr, worst, maps, hdr);
        QVERIFY(worst < 0.1);
        QCOMPARE(maps, hdr);
    }

    void isoGainMapMetadataIsReadWhole() {
        // An iPhone's tmap (iOS 18), as it is in the HEIC after its version
        // byte: one channel, the base's colour space, separate denominators.
        const QByteArray apple = QByteArray::fromHex(
            "000000004000000000000f42400024543b000f424000000000000f42400024543b000f4240000cb9c0000f42400000000a000f4240"
            "0000000a000f4240");
        GainMap map;
        QVERIFY(isoGainMapMetadata(apple, &map));
        QCOMPARE(map.kind, QString("iso"));
        QVERIFY(map.useBaseColourSpace && !map.backward);
        QCOMPARE(map.baseHeadroom, 0.0);
        QVERIFY(std::abs(map.alternateHeadroom - 2.380859) < 1e-6);
        for (int c = 0; c < 3; ++c) {
            QCOMPARE(map.gainMin[c], 0.0);
            QVERIFY(std::abs(map.gainMax[c] - 2.380859) < 1e-6);
            QVERIFY(std::abs(map.gamma[c] - 0.833984) < 1e-6);
            QVERIFY(std::abs(map.baseOffset[c] - 1e-5) < 1e-12 && std::abs(map.alternateOffset[c] - 1e-5) < 1e-12);
        }
        // Its weight: none at SDR, half at half its headroom, all from it on.
        QCOMPARE(map.weight(0), 0.0);
        QVERIFY(std::abs(map.weight(2.380859 / 2) - 0.5) < 1e-9);
        QCOMPARE(map.weight(5), 1.0);
        // Three channels over one denominator (flags 0x80 | 0x08).
        QByteArray multi;
        auto u32 = [&](quint32 v) { multi += char(v >> 24), multi += char(v >> 16), multi += char(v >> 8), multi += char(v); };
        multi += QByteArray("\0\0\0\0", 4);
        multi += char(0x88);
        u32(1000), u32(500), u32(3000);
        for (int c = 0; c < 3; ++c)
            u32(quint32(-250)), u32(1000u * (c + 1)), u32(1000), u32(15), u32(16);
        QVERIFY(isoGainMapMetadata(multi, &map));
        QVERIFY(!map.useBaseColourSpace);
        QCOMPARE(map.baseHeadroom, 0.5);
        QCOMPARE(map.alternateHeadroom, 3.0);
        QCOMPARE(map.gainMin[1], -0.25);
        QCOMPARE(map.gainMax[2], 3.0);
        QCOMPARE(map.alternateOffset[0], 0.016);
        QVERIFY(std::abs(map.weight(1.75) - 0.5) < 1e-9);
        // Cut short anywhere: no.
        for (qsizetype cut = 0; cut < multi.size(); ++cut)
            QVERIFY(!isoGainMapMetadata(multi.left(cut), &map));
        // Apple's legacy: its gain at a code, and its weight, in the log domain.
        GainMap legacy;
        legacy.kind = "apple";
        legacy.appleHeadroom = 8;
        QCOMPARE(legacy.gainOf(0, 0), 0.0);
        QVERIFY(std::abs(legacy.gainOf(1, 0) - 3) < 1e-9);
        QVERIFY(std::abs(std::exp2(legacy.gainOf(0.5, 0)) - (1 + 7 * std::pow((0.5 + 0.099) / 1.099, 1 / 0.45))) < 1e-9);
        QVERIFY(std::abs(legacy.weight(1) - 1.0 / 3) < 1e-9);
        double rgb[3] = {0.5, 0.25, 0.125};
        const double codes[3] = {1, 1, 1};
        legacy.apply(rgb, codes, 1.0 / 3);
        QVERIFY(std::abs(rgb[0] - 1) < 1e-9 && std::abs(rgb[2] - 0.25) < 1e-9);
    }

    // An Ultra HDR JPEG made with libultrahdr (a base, a map at the same
    // size, metadata), then read as the stage reads it and applied by the
    // reference maths: libultrahdr's own decode to linear light at the same
    // boosts agrees.
    void aGainMapIsAppliedAsLibultrahdrDoes() {
        QTemporaryDir dir;
        const int w = 96, h = 64;
        for (const bool multi : {false, true}) {
            QImage base(w, h, QImage::Format_RGB888);
            QImage gain(w, h, multi ? QImage::Format_RGB888 : QImage::Format_Grayscale8);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    base.setPixelColor(x, y, QColor(x * 255 / (w - 1), y * 255 / (h - 1), (x + y) * 255 / (w + h - 2)));
                    const int g = (x / 8 + y / 8) % 2 ? x * 255 / (w - 1) : 255 - y * 255 / (h - 1);
                    // Three channels smooth: chroma at half the size
                    // is upsampled differently by Qt and libultrahdr.
                    gain.setPixelColor(x, y, multi ? QColor(x * 255 / (w - 1), y * 255 / (h - 1), 255 - x * 255 / (w - 1))
                                                   : QColor(g, g, g));
                }
            auto jpeg = [](const QImage &image) {
                QByteArray bytes;
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                image.save(&buffer, "JPG", 100);
                return bytes;
            };
            QByteArray baseJpeg = jpeg(base), gainJpeg = jpeg(gain);
            uhdr_gainmap_metadata_t meta{};
            const float max[3] = {4, 2, 8};
            for (int c = 0; c < 3; ++c) {
                meta.max_content_boost[c] = multi ? max[c] : 4;
                meta.min_content_boost[c] = multi ? 0.75f : 1;
                meta.gamma[c] = multi ? 1.25f : 0.8f;
                meta.offset_sdr[c] = meta.offset_hdr[c] = 1.0f / 64;
            }
            meta.hdr_capacity_min = 1;
            meta.hdr_capacity_max = multi ? 8 : 4;
            meta.use_base_cg = 1;
            uhdr_codec_private_t *encoder = uhdr_create_encoder();
            uhdr_compressed_image_t b{baseJpeg.data(), size_t(baseJpeg.size()), size_t(baseJpeg.size()), UHDR_CG_BT_709,
                                      UHDR_CT_SRGB, UHDR_CR_FULL_RANGE};
            uhdr_compressed_image_t g{gainJpeg.data(), size_t(gainJpeg.size()), size_t(gainJpeg.size()), UHDR_CG_UNSPECIFIED,
                                      UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
            QCOMPARE(int(uhdr_enc_set_compressed_image(encoder, &b, UHDR_BASE_IMG).error_code), int(UHDR_CODEC_OK));
            QCOMPARE(int(uhdr_enc_set_gainmap_image(encoder, &g, &meta).error_code), int(UHDR_CODEC_OK));
            QCOMPARE(int(uhdr_encode(encoder).error_code), int(UHDR_CODEC_OK));
            const uhdr_compressed_image_t *out = uhdr_get_encoded_stream(encoder);
            const QByteArray ultra(static_cast<const char *>(out->data), qsizetype(out->data_sz));
            uhdr_release_encoder(encoder);
            const QString path = dir.path() + (multi ? "/multi.jpg" : "/single.jpg");
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(ultra);
            file.close();

            // As the stage reads it.
            const GainMap map = readGainMap(path);
            QCOMPARE(map.kind, QString("iso"));
            QCOMPARE(map.multichannel(), multi);
            QCOMPARE(map.image.size(), QSize(w, h));
            QVERIFY(std::abs(map.alternateHeadroom - std::log2(meta.hdr_capacity_max)) < 1e-4);
            QVERIFY(std::abs(map.gamma[0] - meta.gamma[0]) < 1e-4);

            // libultrahdr's own decode: the base's codes, and linear light
            // at each boost (1 SDR white).
            auto decode = [&](uhdr_img_fmt_t format, uhdr_color_transfer_t transfer, float boost) {
                uhdr_codec_private_t *decoder = uhdr_create_decoder();
                uhdr_compressed_image_t in{const_cast<char *>(ultra.constData()), size_t(ultra.size()), size_t(ultra.size()),
                                           UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
                uhdr_dec_set_image(decoder, &in);
                uhdr_dec_set_out_img_format(decoder, format);
                uhdr_dec_set_out_color_transfer(decoder, transfer);
                if (boost > 0)
                    uhdr_dec_set_out_max_display_boost(decoder, boost);
                QImage decoded;
                if (uhdr_decode(decoder).error_code == UHDR_CODEC_OK) {
                    const uhdr_raw_image_t *raw = uhdr_get_decoded_image(decoder);
                    const bool half = format == UHDR_IMG_FMT_64bppRGBAHalfFloat;
                    decoded = QImage(static_cast<const uchar *>(raw->planes[0]), int(raw->w), int(raw->h),
                                     qsizetype(raw->stride[0]) * (half ? 8 : 4),
                                     half ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA8888)
                                  .copy();
                }
                uhdr_release_decoder(decoder);
                return decoded;
            };
            // Its base in linear light: at a boost of 1, weight 0.
            const QImage sdr = decode(UHDR_IMG_FMT_64bppRGBAHalfFloat, UHDR_CT_LINEAR, 1);
            QCOMPARE(sdr.size(), QSize(w, h));
            for (const double stops : {1.0, map.alternateHeadroom * 0.75, map.alternateHeadroom}) {
                const QImage hdr = decode(UHDR_IMG_FMT_64bppRGBAHalfFloat, UHDR_CT_LINEAR, float(std::exp2(stops)));
                QCOMPARE(hdr.size(), QSize(w, h));
                double mean = 0, worst = 0;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const auto *s = reinterpret_cast<const qfloat16 *>(sdr.constScanLine(y)) + 4 * x;
                        double rgb[3] = {float(s[0]), float(s[1]), float(s[2])};
                        const QColor m = map.image.pixelColor(x, y);
                        const double codes[3] = {m.redF(), m.greenF(), m.blueF()};
                        map.apply(rgb, codes, map.weight(stops));
                        const auto *p = reinterpret_cast<const qfloat16 *>(hdr.constScanLine(y)) + 4 * x;
                        for (int c = 0; c < 3; ++c) {
                            // Relative to the brighter of the two, and to
                            // 1/64 of SDR white in the darks.
                            const double e = std::abs(rgb[c] - float(p[c])) / std::max({rgb[c], double(p[c]), 1.0 / 64});
                            mean += e;
                            worst = std::max(worst, e);
                        }
                    }
                mean /= 3.0 * w * h;
                qInfo("%s map at %.2f stops (weight %.3f): against libultrahdr, mean %.4f, worst %.4f", multi ? "three-channel" : "one-channel",
                      stops, map.weight(stops), mean, worst);
                QVERIFY(mean < 0.005);
                QVERIFY(worst < 0.03);
            }
        }
    }

    void aDisplaysHeadroomIsItsTargetOverSdrWhite() {
        OutputColour hdr;
        hdr.described = true;
        hdr.referenceLum = 203, hdr.maxLum = 400, hdr.targetMax = 1000;
        QVERIFY(std::abs(hdr.headroom() - 2.30) < 0.01);
        // No target: the maximum.
        hdr.targetMax = 0;
        QVERIFY(std::abs(hdr.headroom() - std::log2(400.0 / 203)) < 1e-9);
        // This MacBook's panel as Hyprland says it: 80 of 80, sRGB.
        OutputColour sdr;
        sdr.described = true;
        sdr.referenceLum = 80, sdr.maxLum = 80, sdr.targetMax = 80;
        const double srgb[8] = {0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.329};
        std::copy(srgb, srgb + 8, sdr.xy);
        QCOMPARE(sdr.headroom(), 0.0);
        QVERIFY(!sdr.wide());
        QVERIFY(sdr.describe().contains("srgb primaries") && sdr.describe().contains("no HDR headroom"));
        sdr.primaries = "display_p3";
        QVERIFY(sdr.wide());
        sdr.primaries = "bt2020";
        QVERIFY(sdr.wide());
        // pinhole.display with nothing to ask (offscreen): SDR; or as told.
        ColourDisplay display;
        QVERIFY(!display.hdr());
        qputenv("PINHOLE_HEADROOM", "2");
        QVERIFY(display.hdr() && display.headroom() == 2);
        qunsetenv("PINHOLE_HEADROOM");
    }

    // What the compositor would have the window be (docs/hdr.md, H3): the
    // preferred description's events read as they come, as an output's.
    void aSurfacesPreferredDescriptionGivesTheHeadroom() {
        const auto *info = static_cast<const wp_image_description_info_v1_listener *>(descriptionInfoListener());
        // PQ, SDR white at 203 nits, shown on a 1000-nit target.
        DescriptionReader hdr;
        info->tf_named(&hdr, nullptr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
        info->primaries_named(&hdr, nullptr, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
        info->luminances(&hdr, nullptr, 50, 10000, 203);
        info->target_luminance(&hdr, nullptr, 50, 1000);
        info->done(&hdr, nullptr);
        QVERIFY(hdr.done && hdr.colour.described);
        QVERIFY(std::abs(hdr.colour.headroom() - 2.30) < 0.01);
        QVERIFY(hdr.colour.wide());
        // This panel's: 80 of 80, no headroom.
        DescriptionReader sdr;
        info->luminances(&sdr, nullptr, 2000, 80, 80);
        info->target_luminance(&sdr, nullptr, 2000, 80);
        info->done(&sdr, nullptr);
        QCOMPARE(sdr.colour.headroom(), 0.0);
        // No target: the maximum, over SDR white.
        DescriptionReader untargeted;
        info->luminances(&untargeted, nullptr, 50, 1000, 203);
        info->done(&untargeted, nullptr);
        QVERIFY(std::abs(untargeted.colour.headroom() - std::log2(1000.0 / 203)) < 1e-9);
    }

    // The stage's colours (stage.h): a photo's codes by the same tables and
    // matrix toSrgb uses, its gamut kept beyond sRGB; clipped, toSrgb's.
    void theStageHasAPhotosOwnColoursAndClippedTheyAreTheWindows() {
        StageColour colour;
        QVERIFY(stageColourOf(testData("display-p3.icc"), colour));
        // P3's green in linear sRGB: the matrix's second column.
        QVERIFY(std::abs(colour.matrix[1] + 0.2249) < 1e-3 && std::abs(colour.matrix[4] - 1.0421) < 1e-3
                && std::abs(colour.matrix[7] + 0.0786) < 1e-3);
        // Every code of a ramp of each primary and grey, clipped and
        // encoded, is toSrgb's within a code.
        QImage codes(256, 4, QImage::Format_RGBX8888);
        for (int v = 0; v < 256; ++v) {
            codes.setPixel(v, 0, qRgb(v, 0, 0));
            codes.setPixel(v, 1, qRgb(0, v, 0));
            codes.setPixel(v, 2, qRgb(0, 0, v));
            codes.setPixel(v, 3, qRgb(v, v / 2, 255 - v));
        }
        QImage converted = codes.copy();
        toSrgb(converted, testData("display-p3.icc"));
        auto encode = [](double l) {
            l = std::clamp(l, 0.0, 1.0);
            return 255 * (l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055);
        };
        int worst = 0;
        bool beyond = false;
        for (int y = 0; y < 4; ++y)
            for (int v = 0; v < 256; ++v) {
                const QRgb in = codes.pixel(v, y), out = converted.pixel(v, y);
                const double light[3] = {colour.linear[0][qRed(in)], colour.linear[1][qGreen(in)], colour.linear[2][qBlue(in)]};
                const int want[3] = {qRed(out), qGreen(out), qBlue(out)};
                for (int k = 0; k < 3; ++k) {
                    const double l = colour.matrix[k * 3] * light[0] + colour.matrix[k * 3 + 1] * light[1] + colour.matrix[k * 3 + 2] * light[2];
                    beyond = beyond || l < -1e-3;
                    worst = std::max(worst, std::abs(int(std::lround(encode(l))) - want[k]));
                }
            }
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
        QVERIFY(beyond);
        // None, and sRGB's: sRGB's curve, nothing between.
        QVERIFY(stageColourOf(QByteArray(), colour));
        QCOMPARE(colour.matrix[0], 1.f);
        QCOMPARE(colour.matrix[1], 0.f);
        QVERIFY(std::abs(colour.linear[1][128] - 0.21586) < 1e-4);
    }

    // The stage only where it is wanted: never on this panel's sRGB, unless
    // forced.
    void theStageIsMadeOnlyWhereTheDisplayShowsMore() {
        ColourDisplay display;
        QVERIFY(!Stage(&display).wanted());
        qputenv("PINHOLE_STAGE", "force");
        QVERIFY(Stage(&display).wanted());
        qputenv("PINHOLE_STAGE", "off");
        qputenv("PINHOLE_HEADROOM", "2");
        QVERIFY(!Stage(&display).wanted());
        qunsetenv("PINHOLE_STAGE");
        QVERIFY(Stage(&display).wanted());
        qunsetenv("PINHOLE_HEADROOM");
    }

    // A 10-bit frame of patches, each a BT.2020 R'G'B' made Y'CbCr (video
    // range), planar or as P010.
    static QVideoFrame hdrFrame(QSize size, bool p010, bool hlg, const std::vector<std::array<double, 3>> &patches) {
        QVideoFrameFormat format(size, p010 ? QVideoFrameFormat::Format_P010 : QVideoFrameFormat::Format_YUV420P10);
        format.setColorSpace(QVideoFrameFormat::ColorSpace_BT2020);
        format.setColorTransfer(hlg ? QVideoFrameFormat::ColorTransfer_STD_B67 : QVideoFrameFormat::ColorTransfer_ST2084);
        format.setColorRange(QVideoFrameFormat::ColorRange_Video);
        QVideoFrame frame(format);
        if (!frame.map(QVideoFrame::WriteOnly))
            return {};
        const int across = int(patches.size());
        auto codes = [&](int x) {
            const auto &p = patches[size_t(std::min(x * across / size.width(), across - 1))];
            const double y = 0.2627 * p[0] + 0.6780 * p[1] + 0.0593 * p[2];
            const double cb = (p[2] - y) / 1.8814, cr = (p[0] - y) / 1.4746;
            return std::array<int, 3>{int(std::lround(64 + 876 * y)), int(std::lround(512 + 896 * cb)), int(std::lround(512 + 896 * cr))};
        };
        const int shift = p010 ? 6 : 0;
        for (int y = 0; y < size.height(); ++y) {
            auto *row = reinterpret_cast<uint16_t *>(frame.bits(0) + frame.bytesPerLine(0) * y);
            for (int x = 0; x < size.width(); ++x)
                row[x] = uint16_t(codes(x)[0] << shift);
        }
        for (int y = 0; y < size.height() / 2; ++y) {
            auto *u = reinterpret_cast<uint16_t *>(frame.bits(1) + frame.bytesPerLine(1) * y);
            auto *v = p010 ? nullptr : reinterpret_cast<uint16_t *>(frame.bits(2) + frame.bytesPerLine(2) * y);
            for (int x = 0; x < size.width() / 2; ++x) {
                const auto c = codes(2 * x);
                if (p010) {
                    u[2 * x] = uint16_t(c[1] << shift);
                    u[2 * x + 1] = uint16_t(c[2] << shift);
                } else {
                    u[x] = uint16_t(c[1]);
                    v[x] = uint16_t(c[2]);
                }
            }
        }
        frame.unmap();
        return frame;
    }

    void hdrVideoFramesAreMadeSdrAsTheFormulasSay() {
        const std::vector<std::array<double, 3>> patches = {
            {0.75, 0.75, 0.75}, {0.5, 0.5, 0.5}, {0.9, 0.3, 0.2}, {0.3, 0.8, 0.3}, {0.2, 0.3, 0.9}, {0.95, 0.95, 0.95}, {0.6, 0.4, 0.7}, {0.1, 0.1, 0.1}};
        const QSize size(256, 32);
        for (const bool hlg : {true, false})
            for (const bool p010 : {false, true}) {
                const QVideoFrame made = toneMapFrame(hdrFrame(size, p010, hlg, patches));
                QVERIFY(made.isValid());
                QCOMPARE(made.size(), size);
                const QImage image = made.toImage();
                for (int k = 0; k < int(patches.size()); ++k) {
                    // Inside each patch, away from its edges' chroma.
                    const int x = k * size.width() / int(patches.size()) + 16;
                    const auto &p = patches[size_t(k)];
                    const QRgb want = sdrOfCodes(p[0], p[1], p[2], hlg, 1000);
                    QVERIFY2(near(image.pixel(x, 16), want, 3),
                             qPrintable(QStringLiteral("%1 %2 patch %3: %4 for %5").arg(hlg ? "HLG" : "PQ", p010 ? "P010" : "planar")
                                            .arg(k).arg(QColor(image.pixel(x, 16)).name(), QColor(want).name())));
                }
            }
        // SDR: none made, the frame goes on as it is.
        QVideoFrameFormat sdr(size, QVideoFrameFormat::Format_YUV420P10);
        sdr.setColorTransfer(QVideoFrameFormat::ColorTransfer_BT709);
        QVERIFY(!toneMapFrame(QVideoFrame(sdr)).isValid());
        // What one costs, on the pool: a phone's 1080p and 4K.
        for (const QSize big : {QSize(1920, 1080), QSize(3840, 2160)}) {
            const QVideoFrame frame = hdrFrame(big, true, true, patches);
            QList<QImage> buffers;
            QElapsedTimer clock;
            clock.start();
            const int rounds = 10;
            for (int i = 0; i < rounds; ++i)
                QVERIFY(toneMapFrame(frame, &buffers).isValid());
            // Each drawn into the one before, which none holds any more.
            QCOMPARE(buffers.size(), 1);
            qInfo("an HLG frame of %dx%d made SDR in %.1f ms", big.width(), big.height(), clock.nsecsElapsed() / 1e6 / rounds);
        }
    }

    void whatDecodingAndColourCostOnRealPhotos() {
        // PINHOLE_DECODE_SAMPLE: a file listing real photos' files, one a
        // line (thumbnails, HEIC originals), for what decoding them and
        // making them sRGB cost on this computer; skipped without.
        if (qEnvironmentVariableIsEmpty("PINHOLE_DECODE_SAMPLE"))
            QSKIP("no PINHOLE_DECODE_SAMPLE");
        QFile list(qEnvironmentVariable("PINHOLE_DECODE_SAMPLE"));
        QVERIFY(list.open(QIODevice::ReadOnly));
        const QByteArray p3 = testData("display-p3.icc");
        struct Cost {
            int count = 0, tagged = 0;
            qint64 decode = 0, convert = 0, whole = 0;
        } thumbs, fulls;
        qint64 fullConvert = 0;
        tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
        tj3Set(tj, TJPARAM_SAVEMARKERS, 4);
        QElapsedTimer clock;
        for (const QByteArray &line : list.readAll().split('\n')) {
            if (line.isEmpty())
                continue;
            const QString path = QFile::decodeName(line);
            if (path.endsWith(".jpg")) {
                // Decoded alone, as before: turbojpeg into RGBX.
                QFile in(path);
                if (!in.open(QIODevice::ReadOnly))
                    continue;
                const QByteArray bytes = in.readAll();
                clock.start();
                if (tj3DecompressHeader(tj, reinterpret_cast<const uchar *>(bytes.constData()), size_t(bytes.size())) != 0)
                    continue;
                QImage image(tj3Get(tj, TJPARAM_JPEGWIDTH), tj3Get(tj, TJPARAM_JPEGHEIGHT), QImage::Format_RGBX8888);
                tj3Decompress8(tj, reinterpret_cast<const uchar *>(bytes.constData()), size_t(bytes.size()), image.bits(), int(image.bytesPerLine()), TJPF_RGBX);
                thumbs.decode += clock.nsecsElapsed();
                uchar *icc = nullptr;
                size_t length = 0;
                tj3GetICCProfile(tj, &icc, &length);
                const QByteArray profile = icc ? QByteArray(reinterpret_cast<const char *>(icc), qsizetype(length)) : QByteArray();
                tj3Free(icc);
                clock.start();
                toSrgb(image, profile);
                thumbs.convert += clock.nsecsElapsed();
                clock.start();
                decodeThumbnail(QFile::encodeName(path), 0);
                thumbs.whole += clock.nsecsElapsed();
                thumbs.count += 1;
                thumbs.tagged += !profile.isEmpty();
            } else {
                // At the size the viewer asks on Jacek's screen, and whole
                // (zoomed in).
                clock.start();
                QImage image = decodePhoto(path, QSize(3456, 2234));
                fulls.whole += clock.nsecsElapsed();
                clock.start();
                QImage whole = decodePhoto(path, QSize());
                fulls.decode += clock.nsecsElapsed();
                if (image.isNull() || whole.isNull())
                    continue;
                // What making it sRGB costs at each size: P3 to sRGB again.
                clock.start();
                toSrgb(image, p3);
                fulls.convert += clock.nsecsElapsed();
                clock.start();
                toSrgb(whole, p3);
                fullConvert += clock.nsecsElapsed();
                fulls.count += 1;
            }
        }
        tj3Destroy(tj);
        if (thumbs.count)
            std::printf("thumbnails: %d (%d tagged): decode %.3f ms, sRGB %.3f ms (%.3f ms each tagged), loader's whole %.3f ms\n",
                        thumbs.count, thumbs.tagged, thumbs.decode / 1e6 / thumbs.count, thumbs.convert / 1e6 / thumbs.count,
                        thumbs.tagged ? thumbs.convert / 1e6 / thumbs.tagged : 0.0, thumbs.whole / 1e6 / thumbs.count);
        if (fulls.count)
            std::printf("full size HEIC: %d at 3456x2234 at most: decode, scale and sRGB %.1f ms, of it sRGB %.1f ms; whole: "
                        "%.1f ms, of it sRGB %.1f ms\n",
                        fulls.count, fulls.whole / 1e6 / fulls.count, fulls.convert / 1e6 / fulls.count,
                        fulls.decode / 1e6 / fulls.count, fullConvert / 1e6 / fulls.count);
    }

    void anImageOfAFileIsTheSizeAnImageOfItsFileWouldBe() {
        // image://file sizes as file:// would: sourceSize and fillMode.
        QTemporaryDir dir;
        QImage picture(400, 300, QImage::Format_RGB32);
        picture.fill(Qt::red);
        QVERIFY(picture.save(dir.path() + "/a.jpg", "JPEG"));
        const QString id = QString::fromUtf8(QUrl::toPercentEncoding(dir.path() + "/a.jpg"));
        FileProvider files;
        QSize size;
        QQuickImageProviderOptions crop, fit;
        crop.setPreserveAspectRatioCrop(true);
        fit.setPreserveAspectRatioFit(true);
        QCOMPARE(files.requestImage(id, &size, QSize(72, 72), crop).size(), QSize(96, 72));
        QCOMPARE(size, QSize(400, 300));
        // Qt's own sums (loadSize): fit and crop cover what is asked, a
        // stretched one fits in it.
        QCOMPARE(files.requestImage(id, &size, QSize(72, 72), fit).size(), QSize(96, 72));
        QCOMPARE(files.requestImage(id, &size, QSize(72, 72), QQuickImageProviderOptions()).size(), QSize(72, 54));
        QCOMPARE(files.requestImage(id, &size, QSize(0, 150), crop).size(), QSize(200, 150));
        QCOMPARE(files.requestImage(id, &size, QSize(), crop).size(), QSize(400, 300));
        QVERIFY(files.requestImage(QStringLiteral("%2Fno%2Fsuch.jpg"), &size, QSize(), crop).isNull());
        QCOMPARE(FileProvider::urlOf(QStringLiteral("/a b/c#d.jpg")).toEncoded(), QByteArray("image://file/%2Fa%20b%2Fc%23d.jpg"));
        QVERIFY(FileProvider::urlOf(QString()).isEmpty());
    }

    void aReadOfALibraryShownNoLongerIsDroppedAndAnotherSourcesPhotosAreOthers() {
        QTemporaryDir first, other;
        makeLibrary(first.path(), {{"a.jpg", 1}, {"b.jpg", 2}});
        makeLibrary(other.path(), {{"a.jpg", 1}, {"b.jpg", 2}, {"c.jpg", 3}});
        Library library;
        QString error;
        QVERIFY(library.open(first.path(), &error));
        ThumbnailLoader loader(&library, 1);
        LibraryReloader reloader(&library, &loader, first.path());
        std::vector<std::vector<int>> moves;
        connect(&reloader, &LibraryReloader::reloaded, this,
                [&](const std::vector<int> &moved, const std::vector<int> &, bool) { moves.push_back(moved); });
        // A read of the first under way as another source's is shown.
        reloader.reload();
        reloader.moveTo(other.path());
        QTRY_COMPARE(library.count(), 3);
        QTest::qWait(50);
        // Only the other's swapped in, and its a.jpg is not the first's.
        QCOMPARE(moves.size(), size_t(1));
        QCOMPARE(moves[0], (std::vector<int>{-1, -1}));
        QCOMPARE(library.dir(), QDir(other.path()).absolutePath());
    }

    // The service showing no source (its sources' plugins off): its library
    // is library/none, which pinholed may not have made. The window shows
    // nothing then, not the last source's photos, and the photos come back
    // with the source.
    void aLibraryThatIsNotThereIsAnEmptyOne() {
        QTemporaryDir root;
        const QString icloud = root.path() + "/library/icloud/me", none = root.path() + "/library/none";
        QVERIFY(QDir().mkpath(icloud));
        makeLibrary(icloud, {{"a.jpg", 1}, {"b.jpg", 2}}, false, "icloud:me");
        Library library;
        QString error;
        QVERIFY(library.open(icloud, &error));
        ThumbnailLoader loader(&library, 1);
        LibraryReloader reloader(&library, &loader, icloud);
        std::vector<std::vector<int>> moves;
        connect(&reloader, &LibraryReloader::reloaded, this,
                [&](const std::vector<int> &moved, const std::vector<int> &, bool) { moves.push_back(moved); });
        int others = 0;
        connect(&reloader, &LibraryReloader::otherSource, this, [&] { ++others; });
        reloader.moveTo(none);
        QTRY_COMPARE(moves.size(), size_t(1));
        QCOMPARE(library.count(), 0);
        QCOMPARE(library.dir(), QDir(none).absolutePath());
        QCOMPARE(moves[0], (std::vector<int>{-1, -1}));
        QCOMPARE(others, 1);
        // Read again while still not there: still nothing, quietly.
        reloader.reload();
        QTRY_COMPARE(moves.size(), size_t(2));
        QCOMPARE(library.count(), 0);
        // The source back: its photos.
        reloader.moveTo(icloud);
        QTRY_COMPARE(library.count(), 2);
        QCOMPARE(library.source(), QString("icloud:me"));
        // One there that cannot be read is no empty one: what is shown stays.
        QVERIFY(QDir().mkpath(root.path() + "/broken"));
        write(root.path() + "/broken/library.sqlite", "not a database");
        reloader.moveTo(root.path() + "/broken");
        QTest::qWait(300);
        QCOMPARE(library.count(), 2);
    }

    void aLibrarySaysWhoseItIsAndAMovedOneIsTheSameOne() {
        // Two folders' libraries named alike (as a-b/c and a/b-c both were
        // folder/a-b-c once), and the first one moved elsewhere.
        QTemporaryDir root;
        const QString first = root.path() + "/one/library/folder/a-b-c", other = root.path() + "/two/library/folder/a-b-c",
                      moved = root.path() + "/three/library/folder/a-b-c";
        for (const QString &dir : {first, other, moved})
            QVERIFY(QDir().mkpath(dir));
        makeLibrary(first, {{"x.jpg", 1}, {"y.jpg", 2}}, false, "folder:/a-b/c");
        makeLibrary(other, {{"x.jpg", 1}, {"y.jpg", 2}}, false, "folder:/a/b-c");
        makeLibrary(moved, {{"x.jpg", 1}, {"y.jpg", 2}}, false, "folder:/a-b/c");
        Library library;
        QString error;
        QVERIFY(library.open(first, &error));
        QCOMPARE(library.source(), QString("folder:/a-b/c"));
        ThumbnailLoader loader(&library, 1);
        LibraryReloader reloader(&library, &loader, first);
        std::vector<std::vector<int>> moves;
        connect(&reloader, &LibraryReloader::reloaded, this,
                [&](const std::vector<int> &moved, const std::vector<int> &, bool) { moves.push_back(moved); });
        int others = 0;
        connect(&reloader, &LibraryReloader::otherSource, this, [&] { ++others; });
        // The same source's library moved: the photos follow.
        reloader.moveTo(moved);
        QTRY_COMPARE(moves.size(), size_t(1));
        QCOMPARE(moves[0], (std::vector<int>{0, 1}));
        QCOMPARE(others, 0);
        // Another source's, though named alike: nothing carries over.
        reloader.moveTo(other);
        QTRY_COMPARE(moves.size(), size_t(2));
        QCOMPARE(moves[1], (std::vector<int>{-1, -1}));
        QCOMPARE(others, 1);
        QCOMPARE(library.source(), QString("folder:/a/b-c"));
    }

    void aMediumAnswerFromBeforeAReloadIsNotTakenForAnotherPhoto() {
        Library library;
        ThumbnailLoader loader(&library, 1);
        QSignalSpy asked(&loader, &ThumbnailLoader::mediumNeeded);
        loader.setWanted({mediumKey(3, 0)});
        QCOMPARE(asked.size(), 1);
        const quint64 before = asked.first().at(1).value<quint64>();
        QCOMPARE(before, loader.generation());

        // The library is read again; the answer for position 3 comes after,
        // when 3 is another photo: it must not count for that one.
        loader.reload([] {});
        QVERIFY(loader.generation() != before);
        loader.setMediumUnavailable(3, before);
        loader.setWanted({mediumKey(3, 0)});
        QCOMPARE(asked.size(), 2);
        QCOMPARE(asked.last().at(1).value<quint64>(), loader.generation());

        // An answer of this generation counts.
        loader.setMediumUnavailable(3, loader.generation());
        loader.setWanted({mediumKey(3, 0)});
        QCOMPARE(asked.size(), 2);
    }

    void fullGridKeysDoNotAliasMosaicsAndOldAnswersAreIgnored() {
        for (int i : {0, 1, 49000}) for (int level = 0; level < 4; ++level) {
            const auto key = fullKey(i, level);
            QVERIFY(isFullKey(key));
            QVERIFY(!isPageKey(key) && !isMediumKey(key));
            QCOMPARE(keyIndex(key), i);
            QCOMPARE(keyLevel(key), level);
            QVERIFY(isPageKey(pageKey(i)) && !isFullKey(pageKey(i)));
        }
        QCOMPARE(fullShortEdge(fullLevelFor(1500)), 2048);
        QCOMPARE(fullShortEdge(fullLevelFor(2500)), 4096);
        Library library;
        ThumbnailLoader loader(&library, 1);
        QSignalSpy asked(&loader, &ThumbnailLoader::fullNeeded);
        const auto before = loader.generation();
        loader.setWanted({fullKey(3, 2), fullKey(3, 1)});
        QCOMPARE(asked.size(), 1);
        loader.setWanted({fullKey(3, 2)});
        QCOMPARE(asked.size(), 1);
        loader.reload([] {});
        loader.setFullPath(3, "/old/photo.png", before);
        loader.setFullUnavailable(3, before);
        loader.setWanted({fullKey(3, 2)});
        QCOMPARE(asked.size(), 2);
        loader.setFullUnavailable(3, loader.generation());
        loader.setWanted({fullKey(3, 2)});
        QCOMPARE(asked.size(), 2);
        loader.clearFull(3, loader.generation());
        loader.setWanted({fullKey(3, 2)});
        QCOMPARE(asked.size(), 3);
    }

    void aPortraitPngSharpensItsSquareCropWithoutDecodingAnUnneededOriginalSize() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"screenshot", 1}});
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "sharp");
            db.setDatabaseName(dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE assets SET thumb_w=192, thumb_h=480"));
        }
        QSqlDatabase::removeDatabase("sharp");
        QImage png(1200, 3000, QImage::Format_RGB32);
        png.fill(QColor(12, 40, 80));
        // Real PNG content, even with a misleading file extension.
        const QString path = dir.path() + "/screenshot.jpg";
        QVERIFY(png.save(path, "PNG"));
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        QSignalSpy ready(&loader, &ThumbnailLoader::ready);
        loader.setFullPath(0, QFile::encodeName(path), loader.generation());
        loader.setWanted({fullKey(0, 3)});
        QTRY_VERIFY(!ready.isEmpty());
        auto decoded = loader.takeReady(1);
        QCOMPARE(decoded.size(), size_t(1));
        // 1024 across the crop, rather than 1024 along the whole portrait.
        QCOMPARE(decoded[0].image.size(), QSize(1024, 2560));
        QCOMPARE(decoded[0].image.pixelColor(500, 1000), QColor(12, 40, 80));
        ready.clear();
        loader.setMediumPath(0, QFile::encodeName(path), loader.generation());
        loader.setWanted({mediumKey(0, 1)});
        QTRY_VERIFY(!ready.isEmpty());
        decoded = loader.takeReady(1);
        QCOMPARE(decoded.size(), size_t(1));
        QCOMPARE(decoded[0].image.size(), QSize(600, 1500));
    }

    void anOversizedGridCellUpgradesAPreviewToTheFullPhoto_data() {
        QTest::addColumn<int>("previewMode");
        QTest::newRow("small-jpeg") << 0;
        QTest::newRow("no-medium-like-icloud-screenshots") << 1;
        QTest::newRow("png-preview") << 2;
        QTest::newRow("broken-preview") << 3;
    }

    void anOversizedGridCellUpgradesAPreviewToTheFullPhoto() {
        QFETCH(int, previewMode);
        // A real scene graph on a virtual display: no user photos or service.
        if (QGuiApplication::platformName() == "offscreen")
            QSKIP("Run this render regression on Xvfb with QT_QPA_PLATFORM=xcb");
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"screenshot", 1}});
        QImage full(1200, 3000, QImage::Format_RGB32);
        for (int y = 0; y < full.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(full.scanLine(y));
            for (int x = 0; x < full.width(); ++x) line[x] = (x / 3) % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
        }
        const QString original = dir.path() + "/full.png", preview = dir.path() + "/medium.jpg";
        QVERIFY(full.save(original));
        if (previewMode == 3) write(preview, "damaged preview");
        else QVERIFY(full.scaled(300, 750, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(preview, previewMode == 2 ? "PNG" : "JPEG"));
        QVERIFY(full.scaled(192, 480, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(dir.path() + "/thumb.jpg"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "sharp");
            db.setDatabaseName(dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE assets SET thumb_path='thumb.jpg', thumb_w=192, thumb_h=480"));
        }
        QSqlDatabase::removeDatabase("sharp");
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 2);
        QQuickWindow window;
        window.resize(900, 700);
        PhotoGrid grid(window.contentItem());
        grid.setSize(QSizeF(900, 700));
        grid.setSources(&library, &loader, nullptr);
        grid.setColumnsNow(1);
        int fullRequests = 0;
        connect(&loader, &ThumbnailLoader::mediumNeeded, this, [&](const QList<int> &indexes, quint64 generation) {
            for (int index : indexes) {
                if (previewMode == 1) loader.setMediumUnavailable(index, generation);
                else loader.setMediumPath(index, QFile::encodeName(preview), generation);
            }
            grid.update();
        });
        connect(&loader, &ThumbnailLoader::fullNeeded, this, [&](const QList<int> &indexes, quint64 generation) {
            for (int index : indexes) {
                ++fullRequests;
                loader.setFullPath(index, QFile::encodeName(original), generation);
            }
            grid.update();
        });
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE_WITH_TIMEOUT(fullRequests, 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(loader.decodedCount(1) >= (previewMode == 1 ? 1 : 2), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(grid.mediumOnScreen().second == 1, 10000);
        // The full square crop contains the high-frequency stripes; the
        // compressed, upscaled preview has lost most of their contrast.
        QImage sharp;
        double contrast = 0;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            sharp = window.grabWindow();
            if (sharp.isNull()) return false;
            int low = 255, high = 0;
            const int y = sharp.height() / 2;
            for (int x = 100; x < std::min(800, sharp.width()); ++x) {
                const int value = qGray(sharp.pixel(x, y));
                low = std::min(low, value); high = std::max(high, value);
            }
            contrast = high - low;
            return contrast > 220;
        })(), 10000);
        window.hide();
    }

    void sustainedSideWheelCrossesTheWholeZoomRange_data() {
        QTest::addColumn<int>("start"); QTest::addColumn<int>("angle"); QTest::addColumn<int>("end");
        QTest::newRow("all-the-way-in") << 29 << 30 << 1;
        QTest::newRow("all-the-way-out") << 1 << -30 << 29;
        QTest::newRow("default-to-in") << 7 << 30 << 1;
        QTest::newRow("default-to-out") << 7 << -30 << 29;
    }

    void sustainedSideWheelCrossesTheWholeZoomRange() {
        QFETCH(int, start); QFETCH(int, angle); QFETCH(int, end);
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}});
        Library library; QString error; QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        ClickableGrid grid; grid.setSize({1400, 900}); grid.setSources(&library, &loader, nullptr);
        grid.setColumnsNow(start);
        // Continuous high-resolution packets, with no rest that could start
        // another gesture. One turn must not get stuck at its first level.
        for (int i = 0; i < 120; ++i) { grid.sideWheel(angle); grid.advance(1.0 / 120); }
        for (int i = 0; i < 60; ++i) grid.advance(1.0 / 120);
        QVERIFY2(std::abs(grid.zoomStride() - 1400. / end) < 0.1,
                 qPrintable(QString("continuous turn stopped at stride %1; wanted %2").arg(grid.zoomStride()).arg(1400. / end)));
        QTest::qWait(230);
        for (int i = 0; i < 80; ++i) grid.advance(1.0 / 120);
        QCOMPARE(grid.columns(), end);
        QVERIFY(!grid.zooming());
    }

    void aSideWheelReversesWithoutARestAndDoesNotStoreOvershootAtTheEnds() {
        QTemporaryDir dir; makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}});
        Library library; QString error; QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        ClickableGrid grid; grid.setSize({1400, 900}); grid.setSources(&library, &loader, nullptr);
        grid.setColumnsNow(7);
        // One large packet, followed by continued turning at the limit.
        grid.sideWheel(12000);
        for (int i = 0; i < 60; ++i) { grid.sideWheel(120); grid.advance(1.0 / 120); }
        QVERIFY(std::abs(grid.zoomStride() - 1400) < 1);
        grid.sideWheel(-30);
        for (int i = 0; i < 15; ++i) grid.advance(1.0 / 120);
        QVERIFY(grid.zoomStride() < 1380); // the first reverse packet already takes effect
        for (int i = 0; i < 120; ++i) { grid.sideWheel(-30); grid.advance(1.0 / 120); }
        for (int i = 0; i < 60; ++i) grid.advance(1.0 / 120);
        QVERIFY(std::abs(grid.zoomStride() - 1400. / 29) < 0.1);
        QTest::qWait(230);
        for (int i = 0; i < 80; ++i) grid.advance(1.0 / 120);
        QCOMPARE(grid.columns(), 29);
    }

    void shortSideWheelTurnsKeepTheirFractionalTicksAndSettleToALevel() {
        QTemporaryDir dir; makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}});
        Library library; QString error; QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        ClickableGrid grid; grid.setSize({1400, 900}); grid.setSources(&library, &loader, nullptr);
        grid.setColumnsNow(7);
        for (int i = 0; i < 30; ++i) { grid.sideWheel(15); grid.advance(1.0 / 120); }
        QTest::qWait(230);
        for (int i = 0; i < 120; ++i) grid.advance(1.0 / 120);
        QCOMPARE(grid.columns(), 3);
        // A short deliberate reverse turn can leave the end on release.
        grid.setColumnsNow(1);
        grid.sideWheel(-120);
        QTest::qWait(230);
        for (int i = 0; i < 120; ++i) grid.advance(1.0 / 120);
        QCOMPARE(grid.columns(), 3);
        grid.sideWheel(3); // an accidental tiny packet settles back
        QTest::qWait(230);
        for (int i = 0; i < 120; ++i) grid.advance(1.0 / 120);
        QCOMPARE(grid.columns(), 3);
    }

    void everyGridLevelClosesWithAFullPhotoRow() {
        QTemporaryDir dir;
        QList<std::pair<QByteArray, qint64>> rows;
        for (int i = 0; i < 93; ++i)
            rows.append({QByteArray::number(i), i * 86400000LL});
        makeLibrary(dir.path(), rows);
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(870, 1740));
        grid.setSources(&library, &loader, nullptr);
        for (int level = 0; level < PhotoGrid::levelCount(); ++level) {
            const int columns = PhotoGrid::columnsAt(level);
            grid.setColumnsNow(columns);
            for (int n = 1; n <= 93; ++n) {
                QVariantList hidden;
                for (int i = n; i < 93; ++i)
                    hidden.append(i);
                grid.setHidden(hidden);
                grid.scrollToStart();
                const double stride = grid.stride();
                const int leading = (columns - n % columns) % columns;
                QCOMPARE(grid.rectOf(0).left(), leading * stride);
                for (int col = 0; col < leading; ++col)
                    QCOMPARE(grid.indexAt((col + 0.5) * stride, stride / 2), -1);
                QCOMPARE(grid.indexAt((leading + 0.5) * stride, stride / 2), 0);
                QCOMPARE(grid.anchor().index, 0);
                grid.scrollToEnd();
                QCOMPARE(grid.rectOf(n - 1).left(), (columns - 1) * stride);
                // If there are at least enough photos for one row, every
                // column of the final row is a real, distinct photo.
                if (n >= columns) {
                    const double y = grid.rectOf(n - 1).center().y();
                    for (int col = 0; col < columns; ++col)
                        QCOMPARE(grid.indexAt((col + 0.5) * stride, y), n - columns + col);
                }
            }
        }
    }

    void theHistoryCoverIsUsefulButNeverASelectablePhoto() {
        QTemporaryDir dir;
        QList<std::pair<QByteArray, qint64>> rows;
        for (int i = 0; i < 93; ++i)
            rows.append({QByteArray::number(i), i});
        makeLibrary(dir.path(), rows);
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        ClickableGrid grid;
        grid.setSize(QSizeF(700, 400));
        grid.setSources(&library, &loader, nullptr);
        grid.scrollToStart();
        QSignalSpy opened(&grid, &PhotoGrid::activated);
        QCOMPARE(grid.indexAt(50, 50), -1);
        grid.setSelecting(true);
        grid.press({50, 50});
        grid.release({50, 50});
        QCOMPARE(grid.selectionCount(), 0);
        QCOMPARE(grid.scrollPosition(), 0.0);
        grid.setSelecting(false);
        grid.press({50, 50});
        grid.release({50, 50});
        for (int frame = 0; frame < 240; ++frame)
            grid.advance(1.0 / 120);
        QVERIFY(grid.atNewest());
        QCOMPARE(opened.count(), 0);
    }

    void aStackIsOneCardAndStaysWithItsPhotosThroughAReload() {
        QTemporaryDir dir, after;
        makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}});
        makeLibrary(after.path(), {{"new", 0}, {"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}});
        Library library, reloaded;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        QVERIFY(reloaded.open(after.path(), &error));
        ThumbnailLoader loader(&library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700)); // 7 columns of 100
        grid.setSources(&library, &loader, nullptr);
        QCOMPARE(grid.count(), 6);

        // b, c and d as one card, where b is; a photo already in a stack
        // stays there, and a stack left with one photo is none.
        grid.setStacks({QVariantList{1, 2, 3}, QVariantList{3, 4}});
        QCOMPARE(grid.count(), 4);
        QCOMPARE(grid.rectOf(3), grid.rectOf(1));
        QCOMPARE(grid.rectOf(4).left(), 500.0);
        QCOMPARE(grid.indexAt(450, 50), 1);
        QCOMPARE(grid.indexAt(550, 50), 4);

        // Selecting the card selects its photos.
        grid.selectAll();
        QCOMPARE(grid.selectionCount(), 6);
        QCOMPARE(grid.selection(), (QList<int>{0, 1, 2, 3, 4, 5}));

        // A photo comes in first: the stack follows its photos.
        grid.rememberAnchor();
        std::vector<int> moved{1, 2, 3, 4, 5, 6};
        loader.reload([&] { library.swap(reloaded); });
        grid.libraryReloaded(moved);
        QCOMPARE(grid.count(), 5);
        QCOMPARE(grid.rectOf(4), grid.rectOf(2));
        QCOMPARE(grid.selection(), (QList<int>{1, 2, 3, 4, 5, 6}));

        // No stacks: a cell to each photo again, the selection kept.
        grid.setStacks({});
        QCOMPARE(grid.count(), 7);
        QCOMPARE(grid.selectionCount(), 6);
        QCOMPARE(grid.rectOf(4).left(), 400.0);
    }

    void aHiddenPhotoHasNoCellLeavesItsStackAndStaysHiddenThroughAReload() {
        QTemporaryDir dir, after;
        makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}});
        makeLibrary(after.path(), {{"new", 0}, {"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}});
        Library library, reloaded;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        QVERIFY(reloaded.open(after.path(), &error));
        ThumbnailLoader loader(&library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700)); // 7 columns of 100
        grid.setSources(&library, &loader, nullptr);

        // a and c hidden: the rest close up, and nothing shows them.
        grid.setHidden({0, 2});
        QCOMPARE(grid.count(), 4);
        QCOMPARE(grid.indexAt(350, 50), 1);
        QCOMPARE(grid.indexAt(450, 50), 3);
        // A hidden photo's cell is where it would be: the next shown.
        QCOMPARE(grid.rectOf(0), grid.rectOf(1));
        QCOMPARE(grid.rectOf(2), grid.rectOf(3));
        QVERIFY(grid.isHidden(2) && !grid.isHidden(3));
        QCOMPARE(grid.hiddenCount(), 2);
        QCOMPARE(grid.rectOf(4).left(), 500.0);
        // Ctrl+A ticks only what shows.
        grid.selectAll();
        QCOMPARE(grid.selection(), (QList<int>{1, 3, 4, 5}));
        QCOMPARE(grid.selectionCount(), 4);
        grid.setSelecting(false);

        // Hidden is out of any stack: b, c, d stack as b and d.
        grid.setStacks(QVariantList{QVariant(QVariantList{1, 2, 3})});
        QCOMPARE(grid.count(), 3);
        QCOMPARE(grid.rectOf(3), grid.rectOf(1));
        QCOMPARE(grid.stackOf(2), -1);
        // A stack whose card is hidden stands where its next photo is.
        grid.setHidden({1});
        QCOMPARE(grid.count(), 4);
        QCOMPARE(grid.indexAt(450, 50), 2);
        QCOMPARE(grid.rectOf(3), grid.rectOf(2));
        grid.setStacks({});

        // Everything hidden: no cells.
        grid.setHidden({0, 1, 2, 3, 4, 5});
        QCOMPARE(grid.count(), 0);
        QCOMPARE(grid.hiddenCount(), 6); // not empty: the window says all are hidden
        QCOMPARE(grid.indexAt(50, 50), -1);
        QVERIFY(grid.rectOf(3).isEmpty());

        // A photo comes in first: what is hidden follows its photos.
        grid.setHidden({0, 2});
        grid.rememberAnchor();
        std::vector<int> moved{1, 2, 3, 4, 5, 6};
        loader.reload([&] { library.swap(reloaded); });
        grid.libraryReloaded(moved);
        QCOMPARE(grid.count(), 5);
        QCOMPARE(grid.indexAt(250, 50), 0);
        QCOMPARE(grid.indexAt(350, 50), 2);
        QCOMPARE(grid.indexAt(450, 50), 4);

        // Nothing hidden: a cell to each photo again.
        grid.setHidden({});
        QCOMPARE(grid.count(), 7);
        QCOMPARE(grid.rectOf(1).left(), 100.0);
    }

    void hidingPhotosKeepsTheViewAtTheNewestEnd() {
        QTemporaryDir dir;
        QList<std::pair<QByteArray, qint64>> rows;
        for (int i = 0; i < 200; ++i)
            rows.append({QByteArray::number(i), i});
        makeLibrary(dir.path(), rows);
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&library, &loader, nullptr);
        QVERIFY(grid.atNewest());
        QVariantList hidden;
        for (int i = 150; i < 200; i += 2)
            hidden.append(i);
        grid.setHidden(hidden);
        // Every other one of the newest hidden: the last row closes up.
        QCOMPARE(grid.scrollFraction(), 1.0);
        QCOMPARE(grid.indexAt(650, 650), 199);
        QCOMPARE(grid.indexAt(550, 650), 197);
        grid.setHidden({});
        QCOMPARE(grid.scrollFraction(), 1.0);
        QCOMPARE(grid.indexAt(650, 650), 199); // the newest always closes the last row
    }

    void theCursorAndAPressStayOnTheirPhotosWhenTheCellsChange() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}});
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        ClickableGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&library, &loader, nullptr);

        // The keyboard's cursor on e: still on e as photos before it go,
        // and on the next one shown when e itself does.
        grid.setCursorShown(true);
        grid.putCursorOn(4);
        QCOMPARE(grid.currentIndex(), 4);
        grid.setHidden({0, 2});
        QCOMPARE(grid.currentIndex(), 4);
        grid.setHidden({0, 2, 4});
        QCOMPARE(grid.currentIndex(), 5);
        // Put on a hidden photo, it goes to where that would be.
        grid.putCursorOn(2);
        QCOMPARE(grid.currentIndex(), 3);
        grid.setHidden({});
        grid.setCursorShown(false);

        // Pressed on d, released after a photo before it was hidden: d
        // opens, not the photo now in its place.
        QSignalSpy opened(&grid, &PhotoGrid::activated);
        grid.press(grid.rectOf(3).center());
        grid.setHidden({0});
        grid.release(grid.rectOf(3).center());
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().first().toInt(), 3);
        // Pressed on d, released after d was hidden: nothing opens.
        grid.setHidden({});
        grid.press(grid.rectOf(3).center());
        grid.setHidden({3});
        grid.release(grid.rectOf(3).center());
        QCOMPARE(opened.count(), 1);
    }

    void revealingAHiddenPhotoGoesToWhereItWouldBe() {
        QTemporaryDir dir;
        QList<std::pair<QByteArray, qint64>> rows;
        for (int i = 0; i < 200; ++i)
            rows.append({QByteArray::number(i), i});
        makeLibrary(dir.path(), rows);
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&library, &loader, nullptr);
        QVariantList hidden;
        for (int i = 10; i < 20; ++i)
            hidden.append(i);
        grid.setHidden(hidden);
        grid.scrollToEnd();
        grid.reveal(15);
        const QRectF r = grid.rectOf(15);
        QCOMPARE(r, grid.rectOf(20));
        QVERIFY(r.top() >= 0 && r.bottom() <= 700);
    }

    void aPluginSeesWhatIsHiddenAndARevealMarksThePhoto() {
        PluginBench bench({{"me.a", {}}});
        ThumbnailLoader loader(&bench.library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&bench.library, &loader, nullptr);
        bench.plugins->setGrid(&grid);
        QSignalSpy changed(bench.host, &PluginHost::hiddenChanged);
        QCOMPARE(bench.js("pinhole.hiddenCount").toInt(), 0);
        grid.setHidden({1});
        QCOMPARE(changed.count(), 1);
        QCOMPARE(bench.js("pinhole.hiddenCount").toInt(), 1);
        QVERIFY(bench.js("pinhole.isHidden(1)").toBool());
        QVERIFY(bench.js("pinhole.isHidden('b')").toBool());
        QVERIFY(!bench.js("pinhole.isHidden(0)").toBool());
        QVERIFY(!bench.js("pinhole.isHidden(7)").toBool());

        // reveal() marks the photo: the window rings it, and the keyboard's
        // cursor is on it (a hidden one: the next shown).
        QSignalSpy revealed(bench.plugins.get(), &Plugins::revealed);
        grid.setCursorShown(true);
        bench.js("pinhole.reveal(1)");
        QCOMPARE(revealed.count(), 1);
        QCOMPARE(revealed.first().first().toInt(), 1);
        QCOMPARE(grid.currentIndex(), 2);
        bench.js("pinhole.reveal('a')");
        QCOMPARE(grid.currentIndex(), 0);
    }

    void aPluginHearsWhatIsHiddenOnceItHasTheLibraryReadAgain() {
        PluginBench bench({{"me.a", {}}});
        ThumbnailLoader loader(&bench.library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&bench.library, &loader, nullptr);
        bench.plugins->setGrid(&grid);
        grid.setHidden({1});
        QVERIFY(bench.js("pinhole.isHidden('b')").toBool());
        bench.js("pinhole.library.changed.connect(() => said.push('changed'));"
                 "pinhole.hiddenChanged.connect(() => said.push('hidden ' + pinhole.isHidden('b') + ' ' + pinhole.library.indexOf('b')))");
        QSignalSpy gridHidden(&grid, &PhotoGrid::hiddenChanged);
        QSignalSpy cacheDir(bench.host, &PluginHost::cacheDirChanged);

        // The same library read again, a photo come in first.
        QVERIFY(QFile::remove(bench.dir.path() + "/library.sqlite"));
        makeLibrary(bench.dir.path(), {{"new", 0}, {"a", 1000}, {"b", 2000}, {"c", 3000}}, false, "folder:/x");
        Library reloaded;
        QString error;
        QVERIFY(reloaded.open(bench.dir.path(), &error));
        const std::vector<int> moved{1, 2, 3};
        grid.rememberAnchor();
        loader.reload([&] { bench.library.swap(reloaded); });
        grid.libraryReloaded(moved);
        // The grid has b hidden where it is now, and says so, its `hidden`
        // too; the same given again is nothing new.
        QCOMPARE(gridHidden.count(), 1);
        QCOMPARE(grid.hidden(), QVariantList{2});
        QVERIFY(grid.isHidden(2) && !grid.isHidden(1));
        grid.setHidden({2});
        QCOMPARE(gridHidden.count(), 1);
        // Plugins hear of it once they have the library as it is now, and
        // not twice.
        QCOMPARE(bench.said(), QStringList());
        bench.viewer->libraryReloaded(moved);
        bench.plugins->libraryReloaded(moved);
        QCOMPARE(bench.said(), QStringList({"changed", "hidden true 2"}));
        // A photo's position from its id is of the library now even before
        // plugins are told: as the grid's news reaches them meanwhile.
        QCOMPARE(bench.js("pinhole.library.indexOf('b')").toInt(), 2);
        // The same source's library: the same cacheDir.
        QCOMPARE(cacheDir.count(), 0);

        // Another source's library: another cacheDir.
        QTemporaryDir other;
        makeLibrary(other.path(), {{"x", 1}}, false, "folder:/y");
        Library theirs;
        QVERIFY(theirs.open(other.path(), &error));
        const std::vector<int> gone{-1, -1, -1, -1};
        loader.reload([&] { bench.library.swap(theirs); });
        QCOMPARE(bench.js("pinhole.library.indexOf('x')").toInt(), 0);
        grid.libraryReloaded(gone);
        bench.viewer->libraryReloaded(gone);
        bench.plugins->libraryReloaded(gone);
        QCOMPARE(cacheDir.count(), 1);
        QCOMPARE(grid.hidden(), QVariantList());
        QCOMPARE(bench.said().last(), QStringLiteral("hidden false -1"));
    }

    void twoPluginsHiddenPhotosFollowThemThroughAReloadUntilEachGivesItsOwn() {
        PluginBench bench({{"me.a", {}}, {"me.b", {}}});
        ThumbnailLoader loader(&bench.library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&bench.library, &loader, nullptr);
        bench.plugins->setGrid(&grid);
        // As the window gathers them (Main.qml's collectPlugins).
        auto gathered = [&](const QVariantList &a, bool aFresh, const QVariantList &b, bool bFresh) {
            return bench.plugins->hiddenOf("me.a", a, aFresh) + bench.plugins->hiddenOf("me.b", b, bFresh);
        };
        // a hidden by one plugin, c by the other.
        QVariantList hidden = gathered({0}, true, {2}, true);
        QCOMPARE(hidden, (QVariantList{0, 2}));
        grid.setHidden(hidden);
        bench.viewer->setHidden(hidden);
        QCOMPARE(grid.count(), 1);

        // A photo comes in first.
        QVERIFY(QFile::remove(bench.dir.path() + "/library.sqlite"));
        makeLibrary(bench.dir.path(), {{"new", 0}, {"a", 1000}, {"b", 2000}, {"c", 3000}}, false, "folder:/x");
        Library reloaded;
        QString error;
        QVERIFY(reloaded.open(bench.dir.path(), &error));
        const std::vector<int> moved{1, 2, 3};
        QSignalSpy positionsMoved(bench.plugins.get(), &Plugins::positionsMoved);
        grid.rememberAnchor();
        loader.reload([&] { bench.library.swap(reloaded); });
        grid.libraryReloaded(moved);
        bench.viewer->libraryReloaded(moved);
        bench.plugins->libraryReloaded(moved);
        QCOMPARE(positionsMoved.count(), 1);
        // The grid and the viewer hide a and c where they are now, and
        // say so as they are read.
        QCOMPARE(grid.hidden(), (QVariantList{1, 3}));
        QCOMPARE(bench.viewer->hidden(), (QVariantList{1, 3}));
        QCOMPARE(grid.count(), 2);
        QSignalSpy gridHidden(&grid, &PhotoGrid::hiddenChanged);
        QSignalSpy viewerHidden(bench.viewer.get(), &Viewer::hiddenChanged);

        // Gathered again before either gives its own: the same.
        QCOMPARE(gathered({0}, false, {2}, false), (QVariantList{1, 3}));
        // One gives its own again, the other not yet: both still right,
        // and nothing new for the grid or the viewer.
        hidden = gathered({1}, true, {2}, false);
        QCOMPARE(hidden, (QVariantList{1, 3}));
        grid.setHidden(hidden);
        bench.viewer->setHidden(hidden);
        QCOMPARE(gridHidden.count(), 0);
        QCOMPARE(viewerHidden.count(), 0);
        // The other gives its own: c shows again.
        hidden = gathered({1}, false, {}, true);
        QCOMPARE(hidden, QVariantList{1});
        grid.setHidden(hidden);
        QCOMPARE(grid.count(), 3);
        // One loaded again (fresh) with the same positions as before is
        // taken at its word.
        QCOMPARE(bench.plugins->hiddenOf("me.b", {2}, true), QVariantList{2});
    }

    void twoPluginsStacksFollowTheirPhotosThroughAReloadUntilEachGivesItsOwn() {
        PluginBench bench({{"me.a", {}}, {"me.b", {}}}, {{"a", 1000}, {"b", 2000}, {"c", 3000}, {"d", 4000}});
        ThumbnailLoader loader(&bench.library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&bench.library, &loader, nullptr);
        bench.plugins->setGrid(&grid);
        auto stack = [](int x, int y) { return QVariant(QVariantList{x, y}); };
        // As the window gathers them (Main.qml's collectPlugins).
        auto gathered = [&](const QVariantList &a, bool aFresh, const QVariantList &b, bool bFresh) {
            return bench.plugins->stacksOf("me.a", a, aFresh) + bench.plugins->stacksOf("me.b", b, bFresh);
        };
        // a and b one card by one plugin, c and d by the other.
        QVariantList stacks = gathered({stack(0, 1)}, true, {stack(2, 3)}, true);
        QCOMPARE(stacks, (QVariantList{stack(0, 1), stack(2, 3)}));
        grid.setStacks(stacks);
        QCOMPARE(grid.count(), 2);

        // A photo comes in first: every position moves one on.
        QVERIFY(QFile::remove(bench.dir.path() + "/library.sqlite"));
        makeLibrary(bench.dir.path(), {{"new", 0}, {"a", 1000}, {"b", 2000}, {"c", 3000}, {"d", 4000}}, false, "folder:/x");
        Library reloaded;
        QString error;
        QVERIFY(reloaded.open(bench.dir.path(), &error));
        const std::vector<int> moved{1, 2, 3, 4};
        grid.rememberAnchor();
        loader.reload([&] { bench.library.swap(reloaded); });
        grid.libraryReloaded(moved);
        bench.plugins->libraryReloaded(moved);
        QCOMPARE(grid.stacks(), (QVariantList{stack(1, 2), stack(3, 4)}));
        QCOMPARE(grid.count(), 3);
        QSignalSpy gridStacks(&grid, &PhotoGrid::stacksChanged);

        // Gathered again before either gives its own (as the window does
        // at once): the same photos, where they are now, never the old
        // positions, which would put b with c.
        stacks = gathered({stack(0, 1)}, false, {stack(2, 3)}, false);
        QCOMPARE(stacks, (QVariantList{stack(1, 2), stack(3, 4)}));
        grid.setStacks(stacks);
        QCOMPARE(gridStacks.count(), 0);
        // One gives its own again, the other not yet: both still right.
        stacks = gathered({stack(1, 2)}, true, {stack(2, 3)}, false);
        QCOMPARE(stacks, (QVariantList{stack(1, 2), stack(3, 4)}));
        grid.setStacks(stacks);
        QCOMPARE(gridStacks.count(), 0);
        // The other gives its own, the same positions as before the reload
        // and so fresh: taken at its word.
        stacks = gathered({stack(1, 2)}, false, {stack(2, 3)}, true);
        QCOMPARE(stacks, (QVariantList{stack(1, 2), stack(2, 3)}));
        // A photo of a stack gone: the rest of it stays together.
        QCOMPARE(bench.plugins->stacksOf("me.a", {stack(1, 2)}, true), (QVariantList{stack(1, 2)}));
        bench.plugins->libraryReloaded({-1, -1, 1, 2, 3});
        QCOMPARE(bench.plugins->stacksOf("me.a", {stack(1, 2)}, false), (QVariantList{QVariant(QVariantList{1})}));
    }

    // Pinhole's Screenshots plugin: what it found is there as it loads,
    // and what it finds of one source's library is never another's.
    // The side panel's mark switches, kept in window.conf as markChoices;
    // the four of before taken from `marks` once, which stays as it was.
    void theMarksSwitchesAreTakenFromTheFourOnceAndKept() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/window.conf";
        auto settings = [&] { return QSettings(path, QSettings::IniFormat); };
        const QStringList four{"pinhole/video", "pinhole.live/live", "pinhole.favourites/favorite", "pinhole/stack"};
        auto shown = [&](MarkChoices &choices) {
            QList<bool> on;
            for (const QString &id : four)
                on << choices.shown(id, id != "pinhole.live/live");
            return on;
        };
        // Jacek's: all but Live. What is not among the four takes its
        // default either way.
        write(path, "[grid]\nmarks=13\n");
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, false, true, true}));
            QVERIFY(!choices.shown("pinhole.live/live", true));
            QVERIFY(choices.shown("me.x/y", true) && !choices.shown("me.x/y", false));
        }
        QCOMPARE(settings().value("grid/marks").toInt(), 13);
        QVERIFY(settings().contains("grid/markChoices"));
        // Read again, the same; a switch turned is kept, and `marks` is not
        // touched.
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, false, true, true}));
            QSignalSpy changed(&choices, &MarkChoices::changed);
            choices.setShown("pinhole.live/live", true);
            choices.setShown("pinhole.live/live", true);
            choices.setShown("me.x/y", false);
            QCOMPARE(changed.count(), 2);
            QCOMPARE(choices.revision(), 2);
        }
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, true, true, true}));
            QVERIFY(!choices.shown("me.x/y", true));
        }
        QCOMPARE(settings().value("grid/marks").toInt(), 13);
        // Once taken, `marks` changed by an older Pinhole is not taken again.
        {
            QSettings older(path, QSettings::IniFormat);
            older.setValue("grid/marks", 0);
        }
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, true, true, true}));
        }
        // All four, none.
        write(path, "[grid]\nmarks=15\n");
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, true, true, true}));
        }
        write(path, "[grid]\nmarks=0\n");
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{false, false, false, false}));
            QVERIFY(!choices.shown("pinhole/video", true));
        }
        // No marks kept: each as its default, and nothing written until a
        // switch is turned.
        QVERIFY(QFile::remove(path));
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, false, true, true}));
            QVERIFY(!QFile::exists(path));
            choices.setShown("pinhole/stack", false);
        }
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, false, true, false}));
            QVERIFY(!settings().contains("grid/marks"));
        }
        write(path, "[grid]\nother=1\n");
        {
            MarkChoices choices(path);
            QCOMPARE(shown(choices), (QList<bool>{true, false, true, true}));
            QVERIFY(!settings().contains("grid/markChoices"));
        }
    }

    static std::unique_ptr<PluginMark> mark(const QString &name, const QString &field, const QString &icon = "live",
                                            const QString &colour = "green", int order = 50) {
        auto m = std::make_unique<PluginMark>();
        m->setName(name);
        m->setField(field);
        m->setIcon(icon);
        m->setColour(colour);
        m->setOrder(order);
        return m;
    }
    static QStringList kindIds(const Plugins &plugins) {
        QStringList ids;
        for (const QVariant &kind : plugins.markKinds())
            ids << kind.toMap().value("id").toString();
        return ids;
    }

    // Pinhole's marks and the plugins', in their order; at most 4 of a
    // plugin and 32 in all, and one that is not right left out and said.
    void aPluginsMarksAreKindsInTheirOrderWithinTheLimits() {
        QList<std::pair<QString, QJsonObject>> manifests{{"pinhole.live", {}}, {"pinhole.favourites", {}}};
        for (int k = 0; k < 9; ++k)
            manifests.append(std::pair<QString, QJsonObject>(QStringLiteral("me.p%1").arg(k), {}));
        PluginBench bench(manifests);
        Plugins &plugins = *bench.plugins;
        // Pinhole's own: video and stack. Live Photos' and favourites' are
        // their plugins' (Live off until turned on, as plugins/live has it).
        QCOMPARE(kindIds(plugins), (QStringList{"pinhole/video", "pinhole/stack"}));
        auto live = mark("live", "live", "live", "yellow", 30);
        live->setDefaultShown(false);
        plugins.setMarks("pinhole.live", {live.get()});
        auto favourite = mark("favorite", "favorite", "heart", "red", 10);
        plugins.setMarks("pinhole.favourites", {favourite.get()});
        QSignalSpy kindsChanged(&plugins, &Plugins::markKindsChanged);
        const QStringList pinholes{"pinhole.favourites/favorite", "pinhole/video", "pinhole.live/live", "pinhole/stack"};
        QCOMPARE(kindIds(plugins), pinholes);
        const QVariantMap video = plugins.markKinds()[1].toMap();
        QCOMPARE(video.value("plugin").toString(), QString());
        QCOMPARE(video.value("field").toString(), QStringLiteral("video"));
        QCOMPARE(video.value("colour").toString(), QStringLiteral("cyan"));
        QCOMPARE(plugins.markKinds()[2].toMap().value("plugin").toString(), QStringLiteral("pinhole.live"));
        QVERIFY(!plugins.markKinds()[2].toMap().value("defaultShown").toBool());

        // Two of one plugin's, by their order among Pinhole's.
        auto glyph = mark("star", "", QStringLiteral(""), "#ff8800", 25);
        auto lived = mark("lived", "live", "live", "blue");
        plugins.setMarks("me.p0", {glyph.get(), lived.get()});
        QCOMPARE(kindIds(plugins), (QStringList{"pinhole.favourites/favorite", "pinhole/video", "me.p0/star",
                                                "pinhole.live/live", "me.p0/lived", "pinhole/stack"}));
        const QVariantMap star = plugins.markKinds()[2].toMap();
        QCOMPARE(star.value("plugin").toString(), QStringLiteral("me.p0"));
        QCOMPARE(star.value("icon").toString(), QStringLiteral(""));
        QCOMPARE(star.value("field").toString(), QString());
        QVERIFY(star.value("defaultShown").toBool());
        QCOMPARE(kindsChanged.count(), 1);
        // The same again: nothing new. Its order changed: moved.
        plugins.setMarks("me.p0", {glyph.get(), lived.get()});
        QCOMPARE(kindsChanged.count(), 1);
        glyph->setOrder(95);
        QCOMPARE(kindIds(plugins).last(), QStringLiteral("me.p0/star"));
        QCOMPARE(kindsChanged.count(), 2);

        // Not right: left out, and said under the plugin.
        auto noField = mark("a", "date");
        auto noIcon = mark("b", "", "two words");
        auto noColour = mark("c", "", "live", "pink");
        auto noName = mark("d/e", "");
        plugins.setMarks("me.p1", {noField.get(), noIcon.get(), noColour.get(), noName.get()});
        QCOMPARE(kindIds(plugins).size(), 6);
        const QString problem = plugins.at("me.p1").value("problem").toString();
        for (const QString &said : {"no field \"date\"", "icon must be", "no colour \"pink\"", "\"d/e\""})
            QVERIFY2(problem.contains(said), qPrintable(problem));
        // ...and a plugin's own name used twice.
        auto again = mark("lived", "video");
        plugins.setMarks("me.p0", {glyph.get(), lived.get(), again.get()});
        QVERIFY(plugins.at("me.p0").value("problem").toString().contains("two marks are named \"lived\""));
        // Given right: no longer said.
        plugins.setMarks("me.p0", {glyph.get(), lived.get()});
        QCOMPARE(plugins.at("me.p0").value("problem").toString(), QString());
        plugins.setMarks("me.p1", {});

        // Four of a plugin at most, 32 in all.
        std::vector<std::unique_ptr<PluginMark>> many;
        for (int k = 1; k < 9; ++k) {
            QList<QObject *> its;
            for (int n = 0; n < 5; ++n) {
                many.push_back(mark(QStringLiteral("m%1").arg(n), ""));
                its << many.back().get();
            }
            plugins.setMarks(QStringLiteral("me.p%1").arg(k), its);
        }
        QCOMPARE(plugins.markKinds().size(), Plugins::kMaxMarks);
        QVERIFY(plugins.at("me.p1").value("problem").toString().contains("4 marks at most for a plugin"));
        QVERIFY(!kindIds(plugins).contains("me.p1/m4"));
        // 2 of Pinhole's, 2 of p0's and four each of p1 to p7 are 32, and
        // p8's, Favourites' and Live Photos' are left out (here the plugins
        // go by their ids, all in one folder; installed, Pinhole's own come
        // before one's own).
        QVERIFY(kindIds(plugins).contains("me.p7/m3") && !kindIds(plugins).contains("me.p7/m4"));
        QVERIFY(!kindIds(plugins).contains("pinhole.live/live"));
        QVERIFY(!kindIds(plugins).contains("pinhole.favourites/favorite"));
        QVERIFY(plugins.at("me.p8").value("problem").toString().contains("32 marks at most in all"));
        QVERIFY(!kindIds(plugins).contains("me.p8/m0"));
        // Room made: the next plugin's come in, and are no longer said.
        plugins.setMarks("me.p6", {});
        QVERIFY(kindIds(plugins).contains("me.p8/m3") && !kindIds(plugins).contains("pinhole.favourites/favorite"));
        QVERIFY(plugins.at("me.p8").value("problem").toString().contains("4 marks at most for a plugin"));
        QVERIFY(!plugins.at("me.p8").value("problem").toString().contains("32 marks"));
        plugins.setMarks("me.p5", {});
        QVERIFY(kindIds(plugins).contains("pinhole.favourites/favorite") && kindIds(plugins).contains("pinhole.live/live"));
        plugins.setMarks("me.p8", {many[size_t(7 * 5)].get()});
        QCOMPARE(plugins.at("me.p8").value("problem").toString(), QString());
        for (int k = 1; k < 9; ++k)
            plugins.setMarks(QStringLiteral("me.p%1").arg(k), {});
        QCOMPARE(plugins.markKinds().size(), 6);

        // Favourites' and Live Photos' have no stand-in: gone with their
        // plugins', and back.
        auto pluginOf = [&](const QString &id) {
            for (const QVariant &kind : plugins.markKinds())
                if (kind.toMap().value("id") == id)
                    return kind.toMap().value("plugin").toString();
            return QStringLiteral("none");
        };
        QCOMPARE(pluginOf("pinhole.favourites/favorite"), QStringLiteral("pinhole.favourites"));
        plugins.setMarks("pinhole.favourites", {});
        QCOMPARE(pluginOf("pinhole.favourites/favorite"), QStringLiteral("none"));
        QCOMPARE(plugins.markKinds().size(), 5);
        plugins.setMarks("pinhole.favourites", {favourite.get()});
        QCOMPARE(pluginOf("pinhole.favourites/favorite"), QStringLiteral("pinhole.favourites"));
        plugins.setMarks("pinhole.live", {});
        QCOMPARE(pluginOf("pinhole.live/live"), QStringLiteral("none"));
        plugins.setMarks("pinhole.live", {live.get()});
        QCOMPARE(pluginOf("pinhole.live/live"), QStringLiteral("pinhole.live"));

        // A mark gone with its QML goes; a plugin turned off takes its own.
        lived.reset();
        QTRY_COMPARE(kindIds(plugins).size(), 5);
        plugins.setEnabled("me.p0", false);
        QCOMPARE(kindIds(plugins), pinholes);
    }

    // A mark of photos is a bit of each photo's for the grid, followed
    // through the library read again until the plugin gives them again;
    // a mark of a field is the field's.
    void aMarkOfPhotosFollowsThemThroughAReloadUntilGivenAgain() {
        PluginBench bench({{"me.a", {}}, {"me.b", {}}}, {{"a", 1000}, {"b", 2000}, {"c", 3000}});
        Plugins &plugins = *bench.plugins;
        ThumbnailLoader loader(&bench.library, 1);
        PhotoGrid grid;
        grid.setSize(QSizeF(700, 700));
        grid.setSources(&bench.library, &loader, nullptr);
        plugins.setGrid(&grid);
        // As Main.qml has the grid follow them, the plugins' shown.
        auto shownIds = [&] {
            QStringList ids;
            for (const QString &id : kindIds(plugins))
                if (id.startsWith("me."))
                    ids << id;
            return ids;
        };
        auto follow = [&] {
            grid.setMarkKinds(plugins.markKinds());
            grid.setMarksShown(shownIds());
        };
        QObject::connect(&plugins, &Plugins::markKindsChanged, &grid, follow);
        follow();

        auto picked = mark("picked", "");
        picked->setPhotos({0, "c", 7, "nope"});
        auto everyone = mark("everyone", "live");
        plugins.setMarks("me.a", {picked.get()});
        plugins.setMarks("me.b", {everyone.get()});
        QCOMPARE(plugins.marksOf("me.a"), (QVariantMap{{"picked", QVariantList{0, 2}}}));
        QCOMPARE(plugins.marksOf("me.b"), QVariantMap());
        QCOMPARE(grid.marksOn(0), QStringList{"me.a/picked"});
        QCOMPARE(grid.marksOn(1), QStringList());
        QCOMPARE(grid.marksOn(2), QStringList{"me.a/picked"});
        // Turned off in the side panel: not drawn.
        grid.setMarksShown({"pinhole/video"});
        QCOMPARE(grid.marksOn(0), QStringList());
        grid.setMarksShown(shownIds());

        // A photo comes in first, and every photo is a Live Photo now.
        QVERIFY(QFile::remove(bench.dir.path() + "/library.sqlite"));
        makeLibrary(bench.dir.path(), {{"new", 0}, {"a", 1000}, {"b", 2000}, {"c", 3000}}, true, "folder:/x");
        Library reloaded;
        QString error;
        QVERIFY(reloaded.open(bench.dir.path(), &error));
        const std::vector<int> moved{1, 2, 3};
        grid.rememberAnchor();
        loader.reload([&] { bench.library.swap(reloaded); });
        grid.libraryReloaded(moved);
        bench.viewer->libraryReloaded(moved);
        plugins.libraryReloaded(moved);
        // Its photos where they are now; the field's on all four.
        QCOMPARE(plugins.marksOf("me.a"), (QVariantMap{{"picked", QVariantList{1, 3}}}));
        QCOMPARE(grid.marksOn(0), QStringList{"me.b/everyone"});
        QCOMPARE(grid.marksOn(1), (QStringList{"me.a/picked", "me.b/everyone"}));
        QCOMPARE(grid.marksOn(3), (QStringList{"me.a/picked", "me.b/everyone"}));
        // Given again (the same list too, of the library now): as given.
        picked->setPhotos({0, 1});
        QCOMPARE(plugins.marksOf("me.a"), (QVariantMap{{"picked", QVariantList{0, 1}}}));
        QCOMPARE(grid.marksOn(0), (QStringList{"me.a/picked", "me.b/everyone"}));
        QCOMPARE(grid.marksOn(3), QStringList{"me.b/everyone"});
        // Its plugin gone: its marks too, and the grid's bits.
        plugins.setMarks("me.a", {});
        QCOMPARE(grid.marksOn(0), QStringList{"me.b/everyone"});
        plugins.setEnabled("me.b", false);
        QCOMPARE(grid.marksOn(0), QStringList());
    }

    // A plugin's QML marks, as the window hands them over (Main.qml):
    // given, changed and gone with the plugin's QML.
    void aPluginsQmlMarksReachTheKinds() {
        PluginBench bench({{"me.a", {}}});
        Plugins::registerTypes();
        QQmlEngine::setObjectOwnership(bench.plugins.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("plugins", bench.engine.newQObject(bench.plugins.get()));
        QQmlComponent component(&bench.engine);
        component.setData(R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    property var picked: [1]
    marks: [
        PluginMark { name: "picked"; text: "Picked"; icon: "★"; colour: "green"; photos: picked },
        PluginMark { name: "loved"; field: "favorite"; icon: "heart"; colour: "blue"; order: 5; defaultShown: false }
    ]
})", QUrl());
        std::unique_ptr<QObject> item(component.create());
        QVERIFY2(item, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(item.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("item", bench.engine.newQObject(item.get()));
        bench.js("plugins.setMarks('me.a', item.marks)");
        QCOMPARE(kindIds(*bench.plugins), (QStringList{"me.a/loved", "pinhole/video", "me.a/picked", "pinhole/stack"}));
        const QVariantMap loved = bench.plugins->markKinds()[0].toMap();
        QCOMPARE(loved.value("colour").toString(), QStringLiteral("blue"));
        QVERIFY(!loved.value("defaultShown").toBool());
        QCOMPARE(bench.plugins->markKinds()[2].toMap().value("text").toString(), QStringLiteral("Picked"));
        QCOMPARE(bench.plugins->marksOf("me.a"), (QVariantMap{{"picked", QVariantList{1}}}));
        bench.js("item.picked = [0, 'c']");
        QCOMPARE(bench.plugins->marksOf("me.a"), (QVariantMap{{"picked", QVariantList{0, 2}}}));
        item.reset();
        QTRY_COMPARE(bench.plugins->markKinds().size(), 2);
    }

    // The kinds' shapes: Pinhole's four, one character, and nothing else;
    // each drawn, plain and lit.
    void aMarksIconIsOneOfPinholesShapesOrOneCharacter() {
        QCOMPARE(markShape("video").builtin, QStringLiteral("video"));
        QCOMPARE(markShape("favorite").builtin, QStringLiteral("heart"));
        QCOMPARE(markShape("heart").builtin, QStringLiteral("heart"));
        QCOMPARE(markShape(QStringLiteral("")).glyph, QStringLiteral(""));
        // Past the first plane, and a flag of two: still one character.
        QCOMPARE(markShape(QStringLiteral("\U000f0a2e")).glyph, QStringLiteral("\U000f0a2e"));
        QCOMPARE(markShape(QStringLiteral("\U0001F1F5\U0001F1F1")).glyph, QStringLiteral("\U0001F1F5\U0001F1F1"));
        for (const QString &bad : {QString(), QStringLiteral("ab"), QStringLiteral(" "), QStringLiteral("Video")})
            QVERIFY2(!markShape(bad).valid(), qPrintable(bad));
        for (const QString &icon : {QStringLiteral("stack"), QStringLiteral("")}) {
            const MarkShape shape = markShape(icon);
            auto lit = [](const QImage &image) {
                int n = 0;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x)
                        n += qAlpha(image.pixel(x, y)) > 128;
                return n;
            };
            QVERIFY2(lit(markImage(shape, 40)) > 40, qPrintable(icon));
            QVERIFY2(lit(plainMark(shape, Qt::green, 40)) > 40, qPrintable(icon));
            QVERIFY2(lit(neonMark(shape, Qt::green, 22).core) > 20, qPrintable(icon));
        }
    }

    void screenshotsHidesWhatItKeptAtOnceAndNeverOneLibrarysAsAnothers() {
        PluginBench bench({{"pinhole.screenshots", {}}});
        // Its library as Find.js asks it, b a screenshot by its name.
        auto screenshot = [](const QString &dir, const QString &id) {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "shots");
            db.setDatabaseName(dir + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE assets ADD COLUMN has_location INTEGER DEFAULT 0"));
            QVERIFY(q.exec("UPDATE assets SET filename = 'Screenshot " + id + ".png' WHERE id = '" + id + "'"));
            db.close();
            db = {};
            QSqlDatabase::removeDatabase("shots");
        };
        screenshot(bench.dir.path(), "b");
        bench.plugins->setSettingOf("pinhole.screenshots", "hide", true);
        PluginStore *cache = bench.plugins->cacheStore();
        cache->set("pinhole.screenshots", "found:folder:/x", QJsonArray{"b"});
        // And of a source taken out since.
        cache->set("pinhole.screenshots", "found:folder:/gone", QJsonArray{"z"});
        // The sources added: both libraries'.
        const QString settings = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/settings.json";
        QDir().mkpath(QFileInfo(settings).absolutePath());
        write(settings, R"({"sources": [{"kind": "folder", "path": "/x"}, {"kind": "folder", "path": "/y"}]})");
        QCOMPARE(Storage::sourceIds(), (QStringList{"folder:/x", "folder:/y"}));
        Keymap keymap;
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        Plugins::registerTypes();
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/screenshots/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.screenshots&load=0");
        std::unique_ptr<QObject> plugin(
            component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(bench.host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        auto list = [&](const char *name) {
            QVariant value = plugin->property(name);
            if (value.canConvert<QJSValue>())
                value = value.value<QJSValue>().toVariant();
            QStringList items;
            for (const QVariant &item : value.toList())
                items << item.toString();
            return items;
        };
        // As it loads, nothing waited for: what was kept, hidden.
        QCOMPARE(list("hidden"), QStringList{"1"});
        QVERIFY(plugin->property("finding").toBool());

        // Another source's library shown while it looks in this one.
        bench.engine.globalObject().setProperty("shots", bench.engine.newQObject(plugin.get()));
        bench.js("shots.idsChanged.connect(() => said.push(shots.ids.join(',')))");
        QTemporaryDir other;
        makeLibrary(other.path(), {{"x", 1}, {"y", 2}}, false, "folder:/y");
        screenshot(other.path(), "y");
        Library theirs;
        QString error;
        QVERIFY(theirs.open(other.path(), &error));
        bench.library.swap(theirs);
        bench.plugins->libraryReloaded({-1, -1, -1});
        QTRY_VERIFY(plugin->property("found").toBool());
        // What it found in the last one was neither shown nor kept as this
        // one's: only what was kept for this one (none), then what it found.
        QCOMPARE(bench.said(), QStringList({"", "y"}));
        QCOMPARE(list("hidden"), QStringList{"1"});
        QJsonObject kept = cache->values("pinhole.screenshots");
        QCOMPARE(kept.value("found:folder:/y").toArray(), QJsonArray{"y"});
        QCOMPARE(kept.value("found:folder:/x").toArray(), QJsonArray{"b"});
        // What it found of a source no longer added is gone.
        QVERIFY(!kept.contains("found:folder:/gone"));
        QVERIFY(bench.js("pinhole.cache['found:folder:/gone']").isUndefined());
        // In its cache's file, and none of it in its settings: only the
        // user's choice is there.
        bench.plugins->flush();
        QFile file(cache->path("pinhole.screenshots"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(), kept);
        QCOMPARE(bench.plugins->settingsOf("pinhole.screenshots"), (QJsonObject{{"hide", true}}));

        // Too many to keep: none kept for that library, rather than a list
        // that would be long to read before the first frame.
        bench.js("shots.keep('found:folder:/y', Array.from({ length: 5001 }, (_, i) => 'p' + i))");
        QVERIFY(!cache->values("pinhole.screenshots").contains("found:folder:/y"));
        bench.js("shots.keep('found:folder:/y', Array.from({ length: 5000 }, (_, i) => 'p' + i))");
        QCOMPARE(cache->values("pinhole.screenshots").value("found:folder:/y").toArray().size(), 5000);

        // A library that does not say whose it is: nothing kept for it, so
        // another such never shares it.
        QTemporaryDir nobodys;
        makeLibrary(nobodys.path(), {{"n", 1}, {"m", 2}}, false, "");
        screenshot(nobodys.path(), "n");
        Library none;
        QVERIFY(none.open(nobodys.path(), &error));
        bench.library.swap(none);
        bench.plugins->libraryReloaded({-1, -1});
        QCOMPARE(plugin->property("keptKey").toString(), QString());
        QTRY_COMPARE(list("hidden"), QStringList{"0"});
        kept = cache->values("pinhole.screenshots");
        QVERIFY(!kept.contains("found:"));
        QCOMPARE(QStringList(kept.keys()), (QStringList{"found:folder:/x", "found:folder:/y"}));
        QCOMPARE(bench.plugins->settingsOf("pinhole.screenshots"), (QJsonObject{{"hide", true}}));
        QVERIFY(QFile::remove(settings));
    }

    void aSweepStaysOnItsPhotosWhenTheCellsChange() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}});
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        ThumbnailLoader loader(&library, 1);
        ClickableGrid grid;
        grid.setSize(QSizeF(700, 700)); // 7 columns of 100
        grid.setSources(&library, &loader, nullptr);
        grid.setSelecting(true);
        // e ticked, then a sweep from b to c: e stays.
        grid.press(grid.rectOf(4).center());
        grid.release(grid.rectOf(4).center());
        QCOMPARE(grid.selection(), QList<int>{4});
        grid.press(grid.rectOf(1).center());
        grid.move(grid.rectOf(2).center());
        QCOMPARE(grid.selection(), (QList<int>{1, 2, 4}));
        // a hidden meanwhile: the sweep goes on from b, and keeps e, though
        // both are in other cells now.
        grid.setHidden({0});
        QCOMPARE(grid.selection(), (QList<int>{1, 2, 4}));
        grid.move(grid.rectOf(3).center()); // d's cell now
        QCOMPARE(grid.selection(), (QList<int>{1, 2, 3, 4}));
        // b, where it began, hidden too: nothing to sweep from, and the
        // selection stays as it is.
        grid.setHidden({0, 1});
        QCOMPARE(grid.selection(), (QList<int>{2, 3, 4}));
        grid.move(QPointF(350, 350));
        QCOMPARE(grid.selectionCount(), 3);
        QCOMPARE(grid.selection(), (QList<int>{2, 3, 4}));
        grid.release(QPointF(350, 350));
        QCOMPARE(grid.selection(), (QList<int>{2, 3, 4}));
    }

    void aWindowGivenAnotherLibraryHasAServiceOfItsOwn() {
        QTemporaryDir run, given, other;
        const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR"), socket = qgetenv("PINHOLE_SOCKET"),
                         pinholed = qgetenv("PINHOLED");
        qunsetenv("PINHOLE_SOCKET");
        qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        // A pinholed that only says how it was started, there and in its
        // log.
        write(run.path() + "/pinholed", "#!/bin/sh\necho \"$@\" | tee \"$XDG_RUNTIME_DIR/started\"\n");
        QVERIFY(QFile::setPermissions(run.path() + "/pinholed", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PINHOLED", (run.path() + "/pinholed").toUtf8());
        auto started = [&] {
            QFile file(run.path() + "/started");
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
        };

        const QString shared = run.path() + "/pinhole.sock";
        QCOMPARE(ServiceClient::socketFor({}), shared);
        const QString own = ServiceClient::socketFor(given.path());
        QVERIFY(own.startsWith(run.path() + "/pinhole-") && own.endsWith(".sock"));
        // The same folder however it is named; another's another.
        QCOMPARE(ServiceClient::socketFor(given.path() + "/"), own);
        QVERIFY(ServiceClient::socketFor(other.path()) != own);
        QVERIFY(ServiceClient::socketFor(other.path()) != shared);
        {
            // None there: the service it starts is its own, on its own
            // socket, never the shared one; one that only reads the
            // library given (--given), in a log of its own.
            const QString logs = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/pinhole/";
            const QString log = logs + QFileInfo(own).completeBaseName() + ".log";
            QFile::remove(log);
            ServiceClient service(given.path());
            QCOMPARE(service.socketPath(), own);
            QTRY_VERIFY(!started().isEmpty());
            QCOMPARE(started(), "--socket " + own + " --library " + given.path() + " --given");
            QTRY_COMPARE(read(log).trimmed(), started());
            QVERIFY(QFile::remove(log));
        }
        QVERIFY(QFile::remove(run.path() + "/started"));
        {
            // The library the settings name is the shared service's own: on
            // its socket, and started without it, as from the launcher.
            ServiceClient service(Storage::library());
            QCOMPARE(service.socketPath(), shared);
            QTRY_VERIFY(!started().isEmpty());
            QCOMPARE(started(), "--socket " + shared);
        }
        // PINHOLE_SOCKET, as a test's, is the one whatever the library.
        qputenv("PINHOLE_SOCKET", (run.path() + "/test.sock").toUtf8());
        QCOMPARE(ServiceClient::socketFor(given.path()), run.path() + "/test.sock");

        qputenv("XDG_RUNTIME_DIR", runtime);
        qputenv("PINHOLED", pinholed);
        if (socket.isEmpty())
            qunsetenv("PINHOLE_SOCKET");
        else
            qputenv("PINHOLE_SOCKET", socket);
    }

    void theViewerFetchesItsRowsNeighboursNotTheHiddenOnes() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1000}, {"b", 2000}, {"c", 3000}, {"d", 4000}, {"e", 5000}});
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(dir.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();

        Viewer viewer(&library, &service);
        viewer.setViewport(800, 600);
        viewer.setHidden({1, 2});
        viewer.openAt(0, {});
        auto mediums = [&fake] {
            fake.read();
            QStringList assets;
            for (const QJsonObject &line : std::as_const(fake.asked))
                if (line.value("params").toObject().value("kind").toString() == QLatin1String("medium"))
                    assets << line.value("params").toObject().value("asset").toString();
            return assets;
        };
        QTRY_VERIFY(mediums().contains("d") && mediums().contains("e"));
        QVERIFY(!mediums().contains("b") && !mediums().contains("c"));
        viewer.close();
        qunsetenv("PINHOLE_SOCKET");
    }

    void theViewersRowPassesOverHiddenPhotos() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}});
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/none.sock").toUtf8());
        ServiceClient service(dir.path());
        Viewer viewer(&library, &service);
        viewer.setViewport(800, 600);
        viewer.setHidden({1, 2});
        QCOMPARE(viewer.count(), 3);
        QCOMPARE(viewer.photoAt(1), 3);

        viewer.openAt(0, {});
        QCOMPARE(viewer.place(), 0);
        QCOMPARE(viewer.beside(1), 3);
        QCOMPARE(viewer.beside(-1), -1);
        viewer.step(1);
        QCOMPARE(viewer.index(), 3);
        QCOMPARE(viewer.place(), 1);
        QCOMPARE(viewer.beside(-1), 0);
        viewer.step(-1);
        QCOMPARE(viewer.index(), 0);
        // The ends are the row's.
        viewer.jumpTo(viewer.count() - 1);
        QCOMPARE(viewer.index(), 4);
        viewer.goTo(0);
        QCOMPARE(viewer.index(), 0);

        // A hidden photo opened all the same (a plugin's) lies between the
        // photos either side of it.
        viewer.openAt(2, {});
        QCOMPARE(viewer.beside(-1), 0);
        QCOMPARE(viewer.beside(1), 3);
        viewer.step(1);
        QCOMPARE(viewer.index(), 3);
        viewer.openAt(2, {});
        viewer.step(-1);
        QCOMPARE(viewer.index(), 0);

        // The photos beside the shown one are library positions, not
        // places: the newest ones' neighbours lie past `count`, and are
        // drawn all the same (PhotoViewer.qml).
        viewer.openAt(4, {});
        QCOMPARE(viewer.place(), 2);
        QCOMPARE(viewer.beside(-1), 3);
        QVERIFY(viewer.beside(-1) >= viewer.count());

        // Shown again, they are in the row again.
        viewer.setHidden({});
        QCOMPARE(viewer.count(), 5);
        QCOMPARE(viewer.place(), 4);
        viewer.step(-1);
        QCOMPARE(viewer.index(), 3);

        // A hidden photo past the end of the row has nothing after it, and
        // one before the start nothing before: never a step back the
        // other way.
        viewer.setHidden({3, 4});
        viewer.openAt(4, {});
        QCOMPARE(viewer.beside(1), -1);
        QCOMPARE(viewer.beside(-1), 2);
        viewer.step(1);
        QCOMPARE(viewer.index(), 4);
        viewer.step(-1);
        QCOMPARE(viewer.index(), 2);
        viewer.setHidden({0, 1});
        viewer.openAt(0, {});
        QCOMPARE(viewer.beside(-1), -1);
        viewer.step(-1);
        QCOMPARE(viewer.index(), 0);
        viewer.step(1);
        QCOMPARE(viewer.index(), 2);
        viewer.close();
        qunsetenv("PINHOLE_SOCKET");
    }

    void whatGoesOutIsWhatWasChosenThroughAReload() {
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir before, after;
        makeLibrary(before.path(), {{"a", 1000}, {"b", 2000}}, false, "folder:/before");
        makeLibrary(after.path(), {{"new", 500}, {"a", 1000}, {"b", 2000}}, false, "folder:/after");
        Library library, reloaded;
        QString error;
        QVERIFY(library.open(before.path(), &error));
        QVERIFY(reloaded.open(after.path(), &error));

        Outgoing outgoing(&library, nullptr);
        const Outgoing::Chosen chosen = outgoing.snapshot({1, 7});
        QCOMPARE(chosen.size(), 1); // a position past the end is left out
        library.swap(reloaded);
        // Still asked for as the photos of the library they were chosen in.
        QCOMPARE(chosen.source, QString("folder:/before"));
        QCOMPARE(library.at(1).id, QByteArray("a"));
        QCOMPARE(chosen.first().id, QByteArray("b"));
        QCOMPARE(chosen.first().filename, QString("b.HEIC"));
    }

    // In the compatible form a HEIC goes as the service's JPEG of it
    // (`jpeg`), asked for as the source's photo, under its camera name with
    // .jpg; a JPEG and a video go as they are, and nothing else is asked
    // for. Nothing is written to ~/.cache/pinhole/compatible any more.
    void aHeicGoesOutAsTheServicesJpegUnderItsCameraName() {
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir dir, out;
        makeLibrary(dir.path(), {{"IMG_0001", 1000}, {"IMG_0002", 2000}, {"IMG_0003", 3000}}, false, "folder:/x");
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        // The files the stand-in service answers with: the HEIC's full, the
        // JPEG made of it, a JPEG's full and a video.
        const QString heic = dir.path() + "/full.heic", made = dir.path() + "/full-jpeg.jpg";
        const QString jpeg = dir.path() + "/edit.jpeg", video = dir.path() + "/clip.mov";
        write(heic, "heic");
        write(made, "jpeg of the heic");
        write(jpeg, "jpeg");
        write(video, "mov");
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(dir.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();
        QObject::connect(fake.peer, &QLocalSocket::readyRead, fake.peer, [&] {
            const qsizetype seen = fake.asked.size();
            fake.read();
            for (qsizetype i = seen; i < fake.asked.size(); ++i) {
                const QJsonObject params = fake.asked[i].value("params").toObject();
                const QString asset = params.value("asset").toString(), kind = params.value("kind").toString();
                QJsonObject result;
                if (asset == "IMG_0001" && kind == "full")
                    result = {{"path", heic}, {"type", "public.heic"}};
                else if (asset == "IMG_0001" && kind == "jpeg")
                    result = {{"path", made}, {"type", "public.jpeg"}, {"rendition", "made"}};
                else if (asset == "IMG_0002" && kind == "full")
                    result = {{"path", jpeg}, {"type", "public.jpeg"}};
                else if (asset == "IMG_0003" && kind == "full")
                    result = {{"path", video}, {"type", "com.apple.quicktime-movie"}};
                if (result.isEmpty())
                    fake.send({{"id", fake.asked[i].value("id")}, {"ok", false}, {"error", QJsonObject{{"code", "not_found"}}}});
                else
                    fake.send({{"id", fake.asked[i].value("id")}, {"ok", true}, {"result", result}});
            }
        });
        Outgoing outgoing(&library, &service);
        QStringList files;
        int failed = -1;
        outgoing.prepare(outgoing.snapshot({0, 1, 2}), true, out.path(), [&](const QStringList &f, int n) {
            files = f;
            failed = n;
        });
        QTRY_COMPARE(failed, 0);
        files.sort();
        QCOMPARE(files, (QStringList{out.path() + "/IMG_0001.jpg", out.path() + "/IMG_0002.jpg", out.path() + "/IMG_0003.HEIC"}));
        QCOMPARE(read(out.path() + "/IMG_0001.jpg"), QString("jpeg of the heic"));
        QCOMPARE(read(out.path() + "/IMG_0002.jpg"), QString("jpeg"));
        QStringList asked;
        for (const QJsonObject &line : std::as_const(fake.asked)) {
            const QJsonObject params = line.value("params").toObject();
            asked << params.value("asset").toString() + " " + params.value("kind").toString();
            QCOMPARE(params.value("source").toString(), QString("folder:/x"));
        }
        asked.sort();
        QCOMPARE(asked, (QStringList{"IMG_0001 full", "IMG_0001 jpeg", "IMG_0002 full", "IMG_0003 full"}));
        QVERIFY(!QFileInfo::exists(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/compatible"));
        // The JPEG not made: that photo failed, the rest went.
        QTemporaryDir again;
        QFile::remove(made);
        QObject::disconnect(fake.peer, nullptr, nullptr, nullptr);
        QObject::connect(fake.peer, &QLocalSocket::readyRead, fake.peer, [&] {
            const qsizetype seen = fake.asked.size();
            fake.read();
            for (qsizetype i = seen; i < fake.asked.size(); ++i) {
                const QJsonObject params = fake.asked[i].value("params").toObject();
                if (params.value("kind").toString() == "jpeg")
                    fake.send({{"id", fake.asked[i].value("id")}, {"ok", false},
                               {"error", QJsonObject{{"code", "failing"}, {"message", "magick failed"}}}});
                else
                    fake.send({{"id", fake.asked[i].value("id")}, {"ok", true}, {"result", QJsonObject{{"path", heic}, {"type", "public.heic"}}}});
            }
        });
        failed = -1;
        outgoing.prepare(outgoing.snapshot({0}), true, again.path(), [&](const QStringList &f, int n) {
            files = f;
            failed = n;
        });
        QTRY_COMPARE(failed, 1);
        QVERIFY(files.isEmpty());
        QVERIFY(QDir(again.path()).isEmpty());
        qunsetenv("PINHOLE_SOCKET");
    }

private slots:
    // The set_favorite requests the stand-in service was asked so far.
    static QList<QJsonObject> favouritesAsked(FakeService &fake) {
        fake.read();
        QList<QJsonObject> found;
        for (const QJsonObject &line : std::as_const(fake.asked))
            if (line.value("method").toString() == QLatin1String("set_favorite"))
                found.append(line);
        return found;
    }
    static QJsonArray assetsOf(const QJsonObject &asked) { return asked.value("params").toObject().value("assets").toArray(); }

    // pinhole.setFavorite: the photos change in memory at once (item(),
    // the library's revision), the service is asked for those not so
    // already, whose they are said; the promise has them once it answers,
    // and what it could not set goes back, rejected with its code; put
    // back only on the library it was set on, another shown meanwhile.
    // The lines asked of the stand-in service by method.
    static QList<QJsonObject> askedFor(FakeService &fake, const QString &method) {
        fake.read();
        QList<QJsonObject> found;
        for (const QJsonObject &line : std::as_const(fake.asked))
            if (line.value("method").toString() == method)
                found.append(line);
        return found;
    }
    static QJsonObject sourcesEvent(const QJsonArray &list, const QString &current) {
        return {{"event", "sources"}, {"sources", list}, {"current", current}};
    }

    // The window's sources (Sources): the list, the one shown and the
    // kinds as the service says them; and a plugin's pinhole.sources, which
    // mirrors them and acts only on the kinds its manifest names.
    void pluginSourcesActOnlyOnTheirOwnKinds() {
        PluginBench bench({{"pinhole.icloud", {{"api", 3}, {"sources", QJsonArray{QJsonObject{{"kind", "icloud"}}}}}}},
                          {{"a", 1000}}, true);
        Sources sources(bench.service.get());
        bench.plugins->setSources(&sources);
        QCOMPARE(bench.plugins->at("pinhole.icloud").value("sources").toStringList(), QStringList{"icloud"});
        QCOMPARE(bench.js("pinhole.sources.kinds.join()").toString(), QString("icloud"));
        const QJsonArray list{QJsonObject{{"id", "icloud:me@example.com"}, {"kind", "icloud"}, {"plugin", "pinhole.icloud"},
                                          {"name", "Me"}, {"account", "me@example.com"}, {"count", 3}, {"thumbs", 3}},
                              QJsonObject{{"id", "folder:/x"}, {"kind", "folder"}, {"plugin", "pinhole.folder"}, {"name", "x"},
                                          {"path", "/x"}, {"count", 1}, {"thumbs", 1}}};
        bench.fake.send(sourcesEvent(list, "folder:/x"));
        bench.fake.send({{"event", "kinds"},
                         {"kinds", QJsonArray{QJsonObject{{"kind", "icloud"}, {"plugin", "pinhole.icloud"}, {"on", true}},
                                              QJsonObject{{"kind", "folder"}, {"plugin", "pinhole.folder"}, {"on", false}}}}});
        QTRY_COMPARE(sources.kinds().size(), 2);
        QCOMPARE(sources.kinds()[1].toMap().value("on").toBool(), false);
        QCOMPARE(bench.js("pinhole.sources.list.length").toInt(), 2);
        QCOMPARE(bench.js("pinhole.sources.current").toString(), QString("folder:/x"));
        bench.fake.send({{"event", "health"}, {"state", "offline"}});
        QTRY_COMPARE(bench.js("pinhole.sources.health").toString(), QString("offline"));

        // Another plugin's kind, or a source of it: not_yours, and nothing
        // asked of the service.
        const QString told = ".then(r => said.push('ok ' + JSON.stringify(r)), e => said.push('not ' + e.code))";
        bench.js("pinhole.sources.add('folder', { path: '/y' })" + told);
        bench.js("pinhole.sources.signIn('folder', {})" + told);
        bench.js("pinhole.sources.signOut('folder:/x')" + told);
        bench.js("pinhole.sources.signOut('icloud:nobody')" + told);
        QTRY_COMPARE(bench.said(), QStringList({"not not_yours", "not not_yours", "not not_yours", "not not_found"}));
        QVERIFY(askedFor(bench.fake, "set_source").isEmpty() && askedFor(bench.fake, "sign_out").isEmpty());

        // What could be added: only its own kinds'.
        bench.js("pinhole.sources.discover()");
        QTRY_COMPARE(askedFor(bench.fake, "discover").size(), 1);
        bench.fake.send({{"id", askedFor(bench.fake, "discover")[0].value("id")}, {"ok", true},
                         {"result", QJsonObject{{"kinds", QJsonObject{{"icloud", QJsonObject{{"icloudpd", QJsonArray{"meexamplecom"}}}},
                                                                     {"folder", QJsonObject{{"folders", QJsonArray{}}}}}}}}});
        QTRY_COMPARE(bench.js("Object.keys(pinhole.sources.discovered).join()").toString(), QString("icloud"));
        QCOMPARE(bench.js("pinhole.sources.discovered.icloud.icloudpd[0]").toString(), QString("meexamplecom"));

        // Its own: asked with its kind, answered as the service says.
        QSignalSpy switched(&sources, &Sources::switched);
        bench.js("said.length = 0; pinhole.sources.signIn('icloud', { apple_id: 'me@example.com', password: 'p' })" + told);
        QTRY_COMPARE(askedFor(bench.fake, "sign_in").size(), 1);
        QJsonObject params = askedFor(bench.fake, "sign_in")[0].value("params").toObject();
        QCOMPARE(params.value("kind").toString(), QString("icloud"));
        QCOMPARE(params.value("apple_id").toString(), QString("me@example.com"));
        bench.fake.send({{"id", askedFor(bench.fake, "sign_in")[0].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "wrong_password"}, {"message", "no"}}}});
        QTRY_COMPARE(bench.said(), QStringList({"not wrong_password"}));
        // Signed in: shown already when it answers, and said so.
        bench.js("said.length = 0; pinhole.sources.submitCode('icloud', { apple_id: 'me@example.com', code: '123456' })" + told);
        QTRY_COMPARE(askedFor(bench.fake, "submit_code").size(), 1);
        QCOMPARE(askedFor(bench.fake, "submit_code")[0].value("params").toObject().value("kind").toString(), QString("icloud"));
        bench.fake.send(sourcesEvent(list, "icloud:me@example.com"));
        bench.fake.send({{"id", askedFor(bench.fake, "submit_code")[0].value("id")}, {"ok", true},
                         {"result", QJsonObject{{"state", "signed_in"}}}});
        QTRY_COMPARE(bench.said(), QStringList({"ok {\"state\":\"signed_in\"}"}));
        QCOMPARE(switched.size(), 1);
        QCOMPARE(switched[0][0].toString(), QString("Me"));
        // Signing out of its own: the sources left.
        bench.js("said.length = 0; pinhole.sources.signOut('icloud:me@example.com').then(r => said.push(r.length))");
        QTRY_COMPARE(askedFor(bench.fake, "sign_out").size(), 1);
        bench.fake.send({{"id", askedFor(bench.fake, "sign_out")[0].value("id")}, {"ok", true}, {"result", sourcesEvent(list, "icloud:me@example.com")}});
        QTRY_COMPARE(bench.said(), QStringList({"2"}));

        // A switch turned: pinholed told, which reads plugins.json again.
        bench.plugins->setEnabled("pinhole.icloud", false);
        QTRY_COMPARE(askedFor(bench.fake, "plugins_changed").size(), 1);
    }

    // A kind of pinholed's own (no provider) only in Pinhole's own plugin of
    // it, or in one's own copy of that plugin; a manifest's sources said.
    void aKindOfPinholedsOwnIsOnlyItsOwnPlugins() {
        PluginBench bench({{"pinhole.icloud", {{"api", 3}, {"sources", QJsonArray{QJsonObject{{"kind", "icloud"}}}}}}});
        QTemporaryDir personal;
        const QJsonObject icloud{{"sources", QJsonArray{QJsonObject{{"kind", "icloud"}}}}};
        QVERIFY(Plugins::sourcesProblem(bench.dir.path() + "/plugins/pinhole.icloud", "pinhole.icloud", icloud).isEmpty());
        QVERIFY(Plugins::sourcesProblem(personal.path(), "pinhole.icloud", icloud).isEmpty());
        QVERIFY(Plugins::sourcesProblem(personal.path(), "me.thief", icloud).contains("icloud"));
        QVERIFY(!Plugins::sourcesProblem(personal.path(), "pinhole.icloud", {{"sources", QJsonObject{}}}).isEmpty());
        QVERIFY(!Plugins::sourcesProblem(personal.path(), "pinhole.icloud", {{"sources", QJsonArray{QJsonObject{}}}}).isEmpty());
        QCOMPARE(Plugins::sourceKinds(icloud), QStringList{"icloud"});
    }

    // A source of one's own, run by its provider: what pinholed would refuse
    // (docs/source-providers.md) is said as the plugin is read, and by
    // `pinhole plugin check`. The test source's manifest is fine.
    void aProvidersSourceIsCheckedAsPinholedTakesIt() {
        PluginBench bench({{"pinhole.icloud", {{"api", 3}, {"sources", QJsonArray{QJsonObject{{"kind", "icloud"}}}}}}});
        const QString testSource = QStringLiteral(PINHOLE_I18N "/../../../core/daemon/tests/provider");
        QJsonObject manifest;
        QString main;
        QCOMPARE(Plugins::manifestProblem(testSource, &manifest, &main), QString());
        QTemporaryDir dir;
        write(dir.path() + "/run", "#!/bin/sh\n");
        write(dir.path() + "/data", "");
        QFile(dir.path() + "/run").setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        const auto problem = [&dir](const QJsonObject &source, int api = 3, const QString &id = "me.jpegs") {
            return Plugins::sourcesProblem(dir.path(), id, {{"api", api}, {"sources", QJsonArray{source}}});
        };
        const QJsonObject fine{{"kind", "me.jpegs"}, {"provider", "run"}, {"needs_sign_in", true}, {"accepts", QJsonArray{"jpg", "heic"}},
                               {"rounds_every_secs", 30}};
        QCOMPARE(problem(fine), QString());
        QCOMPARE(problem(QJsonObject{{"kind", "me.jpegs.raw"}, {"provider", "./run"}}), QString());
        const auto with = [&fine](const QString &key, const QJsonValue &value) {
            QJsonObject changed = fine;
            changed.insert(key, value);
            return changed;
        };
        const QList<std::pair<QString, QJsonObject>> wrong = {
            {"api", fine},
            {"its kind must be", with("kind", "other.jpegs")},
            {"its kind must be", with("kind", "me.jpegsx")},
            {"letters, digits", with("kind", "me.jpegs/x")},
            {"pinholed's own", with("kind", "icloud")},
            {"a path in the plugin's folder", with("provider", "../run")},
            {"a path in the plugin's folder", with("provider", "/bin/sh")},
            {"not a program", with("provider", "data")},
            {"not a program", with("provider", "gone")},
            {"true or false", with("needs_sign_in", "yes")},
            {"lower case", with("accepts", QJsonArray{"JPG"})},
            {"lower case", with("accepts", QJsonArray{".jpg"})},
            {"lower case", with("accepts", "jpg")},
            {"5 to 86400", with("rounds_every_secs", 3)},
            {"5 to 86400", with("rounds_every_secs", 2.5)},
        };
        for (const auto &[said, source] : wrong) {
            // The kind icloud is pinholed's own however it is declared.
            const QString id = source.value("kind").toString() == QLatin1String("icloud") ? QStringLiteral("icloud") : QStringLiteral("me.jpegs");
            const QString why = problem(source, said == QLatin1String("api") ? 2 : 3, id);
            QVERIFY2(why.contains(said), qPrintable(said + " not in: " + why));
        }
        QVERIFY(Plugins::sourcesProblem(dir.path(), "me.jpegs", {{"api", 3}, {"sources", QJsonArray{fine, fine}}}).contains("twice"));
    }

    // A step that did not happen after all is dropped: Ctrl Z undoes what was
    // done before it, and the window no longer offers it.
    void aStepDroppedIsAsIfNeverDone() {
        History history;
        QStringList log;
        QSignalSpy dropped(&history, &History::dropped);
        const quint64 first = history.did("me.a", "one", [&] { log << "undo one"; }, [&] { log << "redo one"; });
        const quint64 second = history.did("me.a", "two", [&] { log << "undo two"; }, [&] { log << "redo two"; });
        QVERIFY(first > 0 && second > first);
        history.drop("me.b", second);
        QCOMPARE(dropped.size(), 0);
        history.drop("me.a", second);
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(dropped[0][0].toString(), QString("two"));
        history.undo();
        QCOMPARE(log, QStringList{"undo one"});
        QVERIFY(!history.canUndo());
        history.redo();
        QCOMPARE(log, (QStringList{"undo one", "redo one"}));
        QVERIFY(!history.canRedo());
        // One undone already, dropped: not done again either.
        const quint64 third = history.did("me.a", "three", [&] { log << "undo three"; }, [&] { log << "redo three"; });
        history.undo();
        history.drop("me.a", third);
        QVERIFY(!history.canRedo() && history.canUndo());
    }

    // plugins/icloud against a stand-in service: its rows' words, icloudpd
    // offered when it is here, and signing in with a code, what went wrong
    // said in its panel, over the window until it is done.
    void theICloudPluginSignsInWithACodeAndSaysWhyNot() {
        PluginBench bench({{"pinhole.icloud", {{"api", 3}, {"sources", QJsonArray{QJsonObject{{"kind", "icloud"}}}}}}}, {{"a", 1000}},
                          true);
        Sources sources(bench.service.get());
        bench.plugins->setSources(&sources);
        Keymap keymap;
        Theme theme(bench.dir.path());
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        bench.engine.rootContext()->setContextProperty("theme", &theme);
        Plugins::registerTypes();
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/icloud/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.icloud&load=0");
        std::unique_ptr<QObject> plugin(component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(bench.host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("plugin", bench.engine.newQObject(plugin.get()));
        bench.js("kind = plugin.sources[0]");
        QCOMPARE(bench.js("kind.kind").toString(), QString("icloud"));
        QCOMPARE(bench.js("kind.place({ name: 'Me' })").toString(), QString("iCloud"));
        QCOMPARE(bench.js("kind.noun").toString(), QString("iCloud"));
        // Its row: Sign out on O, asked first; signed out, a line saying so.
        QCOMPARE(bench.js("kind.actions({ id: 'icloud:me@example.com', shown: true, health: 'fine' })[0].key").toString(), QString("O"));
        QCOMPARE(bench.js("kind.note({ shown: true, health: 'fine' })").toString(), QString());
        QCOMPARE(bench.js("kind.note({ shown: true, health: 'needs_sign_in' })").toString(), QString("Sign in again"));
        QCOMPARE(bench.js("kind.actions({ shown: true, health: 'needs_sign_in' }).length").toInt(), 0);
        QCOMPARE(bench.js("kind.ways[0].key").toString(), QString("I"));
        // icloudpd here: offered.
        QCOMPARE(bench.js("kind.offers.length").toInt(), 0);
        bench.js("pinhole.sources.discover()");
        QTRY_COMPARE(askedFor(bench.fake, "discover").size(), 1);
        bench.fake.send({{"id", askedFor(bench.fake, "discover")[0].value("id")}, {"ok", true},
                         {"result", QJsonObject{{"kinds", QJsonObject{{"icloud", QJsonObject{{"icloudpd", QJsonArray{"meexamplecom"}}}}}}}}});
        QTRY_COMPARE(bench.js("kind.offers.length").toInt(), 1);
        QCOMPARE(bench.js("kind.offers[0].title").toString(), QString("iCloud Photos"));

        // Signing in, over the window.
        QVERIFY(!plugin->property("covering").toBool());
        bench.js("kind.signIn({ account: 'me@example.com' })");
        QVERIFY(plugin->property("covering").toBool());
        bench.js("panel = plugin.signInPanel");
        QCOMPARE(bench.js("panel.appleId").toString(), QString("me@example.com"));
        // icloudpd's session for this one: no password needed.
        QVERIFY(bench.js("panel.icloudpdHas('me@example.com') && !panel.icloudpdHas('you@example.com')").toBool());
        bench.js("panel.signIn('me@example.com ', 'secret')");
        QVERIFY(bench.js("panel.busy").toBool());
        QTRY_COMPARE(askedFor(bench.fake, "sign_in").size(), 1);
        QJsonObject params = askedFor(bench.fake, "sign_in")[0].value("params").toObject();
        QCOMPARE(params, (QJsonObject{{"kind", "icloud"}, {"apple_id", "me@example.com"}, {"password", "secret"}}));
        bench.fake.send({{"id", askedFor(bench.fake, "sign_in")[0].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "wrong_password"}, {"message", "no"}}}});
        QTRY_COMPARE(bench.js("panel.error").toString(), QString("Apple did not accept that Apple ID or password."));
        QVERIFY(!bench.js("panel.busy").toBool());
        // A code asked for, a wrong one, then the right one.
        bench.js("panel.signIn('me@example.com', 'secret')");
        QTRY_COMPARE(askedFor(bench.fake, "sign_in").size(), 2);
        bench.fake.send({{"id", askedFor(bench.fake, "sign_in")[1].value("id")}, {"ok", true}, {"result", QJsonObject{{"state", "needs_code"}}}});
        QTRY_VERIFY(bench.js("panel.codeStage && panel.error === ''").toBool());
        bench.js("panel.submitCode('me@example.com', '000000')");
        QTRY_COMPARE(askedFor(bench.fake, "submit_code").size(), 1);
        QCOMPARE(askedFor(bench.fake, "submit_code")[0].value("params").toObject(),
                 (QJsonObject{{"kind", "icloud"}, {"apple_id", "me@example.com"}, {"code", "000000"}}));
        bench.fake.send({{"id", askedFor(bench.fake, "submit_code")[0].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "wrong_code"}, {"message", "no"}}}});
        QTRY_COMPARE(bench.js("panel.error").toString(), QString("That code did not work. The newest one Apple sent is the one to type."));
        QVERIFY(bench.js("panel.needsCode").toBool());
        QSignalSpy switched(&sources, &Sources::switched);
        bench.js("panel.submitCode('me@example.com', '123456')");
        QTRY_COMPARE(askedFor(bench.fake, "submit_code").size(), 2);
        bench.fake.send(sourcesEvent({QJsonObject{{"id", "icloud:me@example.com"}, {"kind", "icloud"}, {"name", "Me"}}},
                                     "icloud:me@example.com"));
        bench.fake.send({{"id", askedFor(bench.fake, "submit_code")[1].value("id")}, {"ok", true}, {"result", QJsonObject{{"state", "signed_in"}}}});
        QTRY_VERIFY(!plugin->property("covering").toBool());
        QCOMPARE(switched.size(), 1);
        // Closed while Apple is asked: its answer is not waited for.
        bench.js("kind.signIn({}); panel.signIn('me@example.com', 'secret'); panel.shown = false");
        QTRY_COMPARE(askedFor(bench.fake, "sign_in").size(), 3);
        bench.fake.send({{"id", askedFor(bench.fake, "sign_in")[2].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "try_again"}, {"message", "no"}}}});
        QTest::qWait(50);
        QCOMPARE(bench.js("panel.error").toString(), QString());
        // Signing out, from its row's button.
        bench.js("kind.actions({ id: 'icloud:me@example.com', shown: true, health: 'fine' })[0].triggered()");
        QTRY_COMPARE(askedFor(bench.fake, "sign_out").size(), 1);
        QCOMPARE(askedFor(bench.fake, "sign_out")[0].value("params").toObject().value("id").toString(), QString("icloud:me@example.com"));
    }

    // plugins/folder against a stand-in service: the biggest folders found
    // offered, with what is in them, and one added and shown as it is
    // chosen.
    void theFoldersPluginOffersTheBiggestFoldersAndAddsOne() {
        PluginBench bench({{"pinhole.folder", {{"api", 3}, {"sources", QJsonArray{QJsonObject{{"kind", "folder"}}}}}}}, {{"a", 1000}},
                          true);
        Sources sources(bench.service.get());
        bench.plugins->setSources(&sources);
        Keymap keymap;
        Theme theme(bench.dir.path());
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        bench.engine.rootContext()->setContextProperty("theme", &theme);
        Plugins::registerTypes();
        // In English, its counted words its own (i18n/en.json).
        Translator english(bench.dir.path() + "/language", PINHOLE_I18N);
        english.setPluginCatalogs({{"pinhole.folder", QStringLiteral(PINHOLE_I18N "/../../../plugins/folder")}});
        english.setEngine(&bench.engine);
        QCoreApplication::installTranslator(&english);
        const auto removed = qScopeGuard([&english] { QCoreApplication::removeTranslator(&english); });
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/folder/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.folder&load=0");
        std::unique_ptr<QObject> plugin(component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(bench.host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("plugin", bench.engine.newQObject(plugin.get()));
        bench.js("kind = plugin.sources[0]");
        QCOMPARE(bench.js("kind.place({ name: 'Pictures/Trips' })").toString(), QString("Pictures/Trips"));
        QCOMPARE(bench.js("kind.note({ shown: true, health: 'offline' })").toString(), QString("Not there: is its disk connected?"));
        QCOMPARE(bench.js("kind.note({ shown: false, health: '' })").toString(), QString());
        QCOMPARE(bench.js("kind.ways[0].key").toString(), QString("F"));
        bench.js("pinhole.sources.discover()");
        QTRY_COMPARE(askedFor(bench.fake, "discover").size(), 1);
        const auto folder = [](const QString &name, int assets, int live, double bytes) {
            return QJsonObject{{"path", "/p/" + name}, {"name", name}, {"assets", assets}, {"live", live}, {"bytes", bytes}};
        };
        bench.fake.send({{"id", askedFor(bench.fake, "discover")[0].value("id")}, {"ok", true},
                         {"result", QJsonObject{{"kinds", QJsonObject{{"folder", QJsonObject{{"folders", QJsonArray{
                             folder("few", 3, 0, 1e6), folder("big", 400, 12, 3.5 * (1 << 30)), folder("mid", 25, 0, 50e6),
                             folder("more", 30, 0, 2.0 * (1 << 30)), folder("most", 900, 0, 12.0 * (1 << 30))}}}}}}}}});
        // Three at most, the biggest first, past the strays.
        QTRY_COMPARE(bench.js("kind.offers.map(o => o.title).join()").toString(), QString("most,big,more"));
        QCOMPARE(bench.js("kind.offers[1].detail").toString(), QString("400 photos and videos · 12 Live Photos · 3.5 GB"));
        QCOMPARE(bench.js("kind.offers[2].detail").toString(), QString("30 photos and videos · 2 GB"));
        QSignalSpy switched(&sources, &Sources::switched);
        bench.js("kind.offers[0].triggered()");
        QTRY_COMPARE(askedFor(bench.fake, "set_source").size(), 1);
        QCOMPARE(askedFor(bench.fake, "set_source")[0].value("params").toObject(), (QJsonObject{{"kind", "folder"}, {"path", "/p/most"}}));
        bench.fake.send(sourcesEvent({QJsonObject{{"id", "folder:/p/most"}, {"kind", "folder"}, {"name", "most"}}}, "folder:/p/most"));
        bench.fake.send({{"id", askedFor(bench.fake, "set_source")[0].value("id")}, {"ok", true},
                         {"result", QJsonObject{{"id", "folder:/p/most"}, {"kind", "folder"}, {"name", "most"}}}});
        QTRY_COMPARE(switched.size(), 1);
        QCOMPARE(switched[0][0].toString(), QString("most"));
        QCOMPARE(sources.error(), QString());
        // One the service will not show: said where it was chosen.
        bench.js("kind.offers[1].triggered()");
        QTRY_COMPARE(askedFor(bench.fake, "set_source").size(), 2);
        bench.fake.send({{"id", askedFor(bench.fake, "set_source")[1].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "bad_request"}, {"message", "no"}}}});
        QTRY_COMPARE(sources.error(), QString("Photos cannot be shown from there."));
    }

    void setFavoriteSetsAtOnceAndPutsBackWhatTheServiceCouldNot() {
        PluginBench bench({{"pinhole.favourites", {{"api", 3}}}}, {{"a", 1000}, {"b", 2000}, {"c", 3000}}, true);
        Favourites favourites(&bench.library, bench.service.get());
        bench.plugins->setFavourites(&favourites);
        const QString told = ".then(r => said.push('set ' + r.changed.join(' ')), e => said.push('not ' + e.code))";
        const int revision = bench.js("pinhole.library.revision").toInt();
        bench.js("pinhole.setFavorite([0, 'b'], true)" + told);
        QVERIFY(bench.library.at(0).favorite && bench.library.at(1).favorite && !bench.library.at(2).favorite);
        QVERIFY(bench.js("pinhole.library.item(1).favorite").toBool());
        QVERIFY(bench.js("pinhole.library.revision").toInt() > revision);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 1);
        const QJsonObject params = favouritesAsked(bench.fake)[0].value("params").toObject();
        QCOMPARE(params.value("assets").toArray(), (QJsonArray{"a", "b"}));
        QVERIFY(params.value("favorite").toBool());
        QCOMPARE(params.value("source").toString(), QString("folder:/x"));
        // Not before the service says.
        QTest::qWait(20);
        QCOMPARE(bench.said(), QStringList());
        bench.fake.send({{"id", favouritesAsked(bench.fake)[0].value("id")}, {"ok", true}, {"result", QJsonObject{{"changed", 2}}}});
        QTRY_COMPARE(bench.said(), QStringList({"set a b"}));

        // Only those not so already; what the service could not set goes back.
        bench.js("said.length = 0; pinhole.setFavorite([0, 1, 2], true)" + told);
        QVERIFY(bench.library.at(2).favorite);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 2);
        QCOMPARE(assetsOf(favouritesAsked(bench.fake)[1]), (QJsonArray{"c"}));
        bench.fake.send({{"id", favouritesAsked(bench.fake)[1].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "unavailable"}, {"message", "no network"}}}});
        QTRY_COMPARE(bench.said(), QStringList({"not unavailable"}));
        QVERIFY(!bench.library.at(2).favorite && bench.library.at(0).favorite && bench.library.at(1).favorite);

        // All so already: nothing asked, and none changed.
        bench.js("said.length = 0; pinhole.setFavorite([0, 1], true)" + told);
        QTRY_COMPARE(bench.said(), QStringList({"set "}));
        QCOMPARE(favouritesAsked(bench.fake).size(), 2);
        // None of them here, or one: not_found, or on the one.
        bench.js("said.length = 0; pinhole.setFavorite([7, 'zz'], false)" + told);
        QTRY_COMPARE(bench.said(), QStringList({"not not_found"}));
        bench.js("said.length = 0; pinhole.setFavorite('zz', false); pinhole.setFavorite(0, false)" + told);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 3);
        QCOMPARE(assetsOf(favouritesAsked(bench.fake)[2]), (QJsonArray{"a"}));
        bench.fake.send({{"id", favouritesAsked(bench.fake)[2].value("id")}, {"ok", true}, {"result", QJsonObject{{"changed", 1}}}});
        QTRY_COMPARE(bench.said(), QStringList({"set a"}));

        // Another source's library shown before the service answers (it
        // shows that one already: other_source): its photos of the same
        // ids are not put back, as they were never set.
        QTemporaryDir otherDir;
        makeLibrary(otherDir.path(), {{"a", 1000}, {"b", 2000}, {"c", 3000}}, false, "folder:/y");
        Library other;
        QString error;
        QVERIFY(other.open(otherDir.path(), &error));
        other.setFavorite(2, true);
        bench.js("said.length = 0; pinhole.setFavorite(2, true)" + told);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 4);
        bench.library.swap(other);
        bench.fake.send({{"id", favouritesAsked(bench.fake)[3].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "other_source"}, {"message", "not shown"}}}});
        QTRY_COMPARE(bench.said(), QStringList({"not other_source"}));
        QVERIFY(bench.library.at(2).favorite);
    }

    // plugins/favourites as the window loads it, over a stand-in service:
    // its mark; the viewer's heart, filled in the accent once a favourite,
    // at once; each press a step undone and done again through the
    // service; why not, said under the heart (or at the foot of the window
    // with no heart showing), and put back; all of them made favourites,
    // or none when they all are, a few or hundreds; undone only on the
    // library it was done in.
    void theFavouritesPluginsHeartSetsAndUndoesAndSaysWhyNot() {
        PluginBench bench({{"pinhole.favourites", {{"api", 3}}}}, {{"a", 1000}, {"b", 2000}, {"c", 3000}}, true);
        Favourites favourites(&bench.library, bench.service.get());
        bench.plugins->setFavourites(&favourites);
        Keymap keymap;
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        Plugins::registerTypes();
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/favourites/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.favourites&load=0");
        std::unique_ptr<QObject> plugin(component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(bench.host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("plugin", bench.engine.newQObject(plugin.get()));
        bench.js("plugin.shownAction.complained.connect(text => said.push('viewer ' + text));"
                 "plugin.tickedAction.complained.connect(text => said.push('ticked ' + text))");
        QSignalSpy notified(bench.host, &PluginHost::notified);
        auto answer = [&bench](int k, const QString &code = {}) {
            const QJsonValue id = favouritesAsked(bench.fake)[k].value("id");
            if (code.isEmpty())
                bench.fake.send({{"id", id}, {"ok", true}, {"result", QJsonObject{{"changed", 1}}}});
            else
                bench.fake.send({{"id", id}, {"ok", false}, {"error", QJsonObject{{"code", code}, {"message", "no"}}}});
        };

        // Its mark: the library's field, a red heart first in the corner, shown.
        QCOMPARE(bench.js("plugin.marks[0].field").toString(), QString("favorite"));
        QCOMPARE(bench.js("plugin.marks[0].icon").toString(), QString("heart"));
        QCOMPARE(bench.js("plugin.marks[0].colour").toString(), QString("red"));
        QCOMPARE(bench.js("plugin.marks[0].order").toInt(), 10);
        QVERIFY(bench.js("plugin.marks[0].defaultShown").toBool());
        QCOMPARE(bench.js("plugin.shownAction.key").toString(), QString("F"));
        QCOMPARE(bench.js("plugin.shownAction.order").toInt(), -20);
        QCOMPARE(bench.js("plugin.tickedAction.place").toString(), QString("selection"));
        QVERIFY(!bench.js("plugin.tickedAction.usable").toBool());

        // F on the photo shown: filled in the accent at once, a step to undo.
        bench.viewer->openAt(0, {});
        QCOMPARE(bench.js("plugin.shownAction.text").toString(), QString(QChar(0xf08a)));
        QCOMPARE(bench.js("plugin.shownAction.textColour").toString(), QString());
        bench.js("plugin.shownAction.triggered()");
        QVERIFY(bench.library.at(0).favorite);
        QCOMPARE(bench.js("plugin.shownAction.text").toString(), QString(QChar(0xf004)));
        QCOMPARE(bench.js("plugin.shownAction.textColour").toString(), QString("accent"));
        QVERIFY(bench.history.canUndo());
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 1);
        QCOMPARE(assetsOf(favouritesAsked(bench.fake)[0]), (QJsonArray{"a"}));
        answer(0);
        // Undone, and done again: through the service each time.
        bench.history.undo();
        QVERIFY(!bench.library.at(0).favorite);
        QCOMPARE(bench.js("plugin.shownAction.textColour").toString(), QString());
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 2);
        QVERIFY(!favouritesAsked(bench.fake)[1].value("params").toObject().value("favorite").toBool());
        answer(1);
        bench.history.redo();
        QVERIFY(bench.library.at(0).favorite);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 3);
        answer(2);
        QTest::qWait(20);
        QCOMPARE(bench.said(), QStringList());

        // What could not be set: said under the heart, and put back; its
        // step goes again, so Ctrl Z undoes what was done before it.
        QSignalSpy droppedSteps(&bench.history, &History::dropped);
        bench.viewer->step(1);
        QCOMPARE(bench.js("plugin.shownAction.textColour").toString(), QString());
        bench.js("plugin.shownAction.triggered()");
        QVERIFY(bench.library.at(1).favorite);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 4);
        answer(3, "read_only");
        QTRY_COMPARE(bench.said(), QStringList({"viewer This library is only being looked at here: its favourites cannot be changed"}));
        QVERIFY(!bench.library.at(1).favorite);
        QCOMPARE(bench.js("plugin.shownAction.textColour").toString(), QString());
        bench.js("plugin.shownAction.triggered()");
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 5);
        answer(4, "needs_sign_in");
        QTRY_COMPARE(bench.said().size(), 2);
        QCOMPARE(bench.said()[1], QString("viewer Not signed in to x"));
        bench.js("plugin.shownAction.triggered()");
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 6);
        answer(5, "unavailable");
        QTRY_COMPARE(bench.said().size(), 3);
        QCOMPARE(bench.said()[2], QString("viewer Could not change favourites in x"));
        QCOMPARE(droppedSteps.size(), 3);
        // Another photo shown before the service said no: not under its
        // heart, which says nothing of it, but at the foot of the window.
        bench.js("plugin.shownAction.triggered()");
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 7);
        bench.viewer->step(1);
        answer(6, "unavailable");
        QTRY_COMPARE(notified.size(), 1);
        QCOMPARE(notified[0][0].toString(), QString("Could not change favourites in x"));
        QCOMPARE(bench.said().size(), 3);
        QCOMPARE(droppedSteps.size(), 4);
        notified.clear();
        bench.viewer->step(-1);

        // Several: all made favourites (those not yet), then none.
        bench.viewer->close();
        QTRY_VERIFY(!bench.viewer->isOpen());
        bench.js("said.length = 0; plugin.toggle([0, 1, 2])");
        QVERIFY(bench.library.at(0).favorite && bench.library.at(1).favorite && bench.library.at(2).favorite);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 8);
        QCOMPARE(assetsOf(favouritesAsked(bench.fake)[7]), (QJsonArray{"b", "c"}));
        answer(7);
        bench.js("plugin.toggle([0, 1, 2])");
        QVERIFY(!bench.library.at(0).favorite && !bench.library.at(1).favorite && !bench.library.at(2).favorite);
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 9);
        QCOMPARE(assetsOf(favouritesAsked(bench.fake)[8]), (QJsonArray{"a", "b", "c"}));
        // With no heart showing (undone in the grid, say): at the foot of the window.
        answer(8, "unavailable");
        QTRY_COMPARE(notified.size(), 1);
        QCOMPARE(notified[0][0].toString(), QString("Could not change favourites in x"));
        QCOMPARE(bench.said(), QStringList());
        QVERIFY(bench.library.at(0).favorite && bench.library.at(1).favorite && bench.library.at(2).favorite);

        // Undone on the library it was done in only: another's photos of
        // the same ids are not touched.
        QTemporaryDir otherDir;
        makeLibrary(otherDir.path(), {{"a", 1000}, {"b", 2000}, {"c", 3000}}, false, "folder:/y");
        Library other;
        QString error;
        QVERIFY(other.open(otherDir.path(), &error));
        bench.library.swap(other);
        QVERIFY(bench.history.canUndo());
        bench.history.undo();
        QTest::qWait(50);
        QCOMPARE(favouritesAsked(bench.fake).size(), 9);
        QVERIFY(!bench.library.at(0).favorite && !bench.library.at(1).favorite);
        bench.history.redo();
        QTest::qWait(50);
        QCOMPARE(favouritesAsked(bench.fake).size(), 9);
        QVERIFY(!bench.library.at(0).favorite && !bench.library.at(1).favorite);
        bench.library.swap(other);
    }

    // Hundreds ticked: by the library's columns, every one of them asked
    // for at once, and all of them back to none.
    void theFavouritesPluginTakesHundredsTickedAtOnce() {
        QList<std::pair<QByteArray, qint64>> photos;
        for (int i = 0; i < 300; ++i)
            photos.append({QByteArray("p") + QByteArray::number(i), 1000 + i});
        PluginBench bench({{"pinhole.favourites", {{"api", 3}}}}, photos, true);
        Favourites favourites(&bench.library, bench.service.get());
        bench.plugins->setFavourites(&favourites);
        Keymap keymap;
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        Plugins::registerTypes();
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/favourites/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.favourites&load=0");
        std::unique_ptr<QObject> plugin(component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(bench.host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("plugin", bench.engine.newQObject(plugin.get()));
        bench.library.setFavorite(10, true);
        bench.js("all = []; for (let i = 0; i < 300; ++i) all.push(i)");
        QVERIFY(!bench.js("plugin.allFavourites(all)").toBool());
        bench.js("plugin.toggle(all)");
        QVERIFY(bench.js("plugin.allFavourites(all)").toBool());
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 1);
        QCOMPARE(assetsOf(favouritesAsked(bench.fake)[0]).size(), 299);
        QVERIFY(!assetsOf(favouritesAsked(bench.fake)[0]).contains("p10"));
        bench.js("plugin.toggle(all)");
        QTRY_COMPARE(favouritesAsked(bench.fake).size(), 2);
        QCOMPARE(assetsOf(favouritesAsked(bench.fake)[1]).size(), 300);
        QVERIFY(!bench.library.at(10).favorite && !bench.library.at(299).favorite);
    }

    void aServiceOfAnotherProtocolIsSaidSoAndItsHealthDoesNotHideIt() {
        QTemporaryDir dir;
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(dir.path());
        QSignalSpy announced(&service, &ServiceClient::announced);
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        QTRY_VERIFY(service.connected());

        // Its own protocol: nothing to say.
        fake.send({{"event", "hello"}, {"protocol", ServiceClient::kProtocol}, {"version", "1.0.0"}});
        fake.send({{"event", "health"}, {"state", "offline"}});
        QTRY_COMPARE(service.health(), QString("offline"));

        // Another one, from a service of another version that came up
        // instead: the window says so, whatever its health.
        fake.peer->disconnectFromServer();
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        QTRY_COMPARE(service.health(), QString("fine"));
        fake.send({{"event", "hello"}, {"protocol", ServiceClient::kProtocol + 1}, {"version", "9.0.0"}});
        fake.send({{"event", "health"}, {"state", "failing"}});
        QTRY_COMPARE(service.health(), QString("incompatible"));
        QTest::qWait(50);
        QCOMPARE(service.health(), QString("incompatible"));
        bool said = false;
        for (const QList<QVariant> &args : std::as_const(announced))
            said |= args.at(0).toString() == QLatin1String("health")
                    && args.at(1).toJsonObject().value("state").toString() == QLatin1String("incompatible");
        QVERIFY(said);
        // Nothing is asked of it: answered so at once, and nothing sent,
        // not a sync either.
        QString code;
        service.call(QStringLiteral("storage"), {}, [&](bool, const QJsonObject &, const QString &c) { code = c; });
        service.sync();
        QTRY_COMPARE(code, QString("incompatible"));
        QTest::qWait(50);
        fake.read();
        QVERIFY(fake.asked.isEmpty());

        // One from before there was a hello: protocol 0. What was asked
        // before it said anything waited, and is refused now.
        fake.peer->disconnectFromServer();
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        QTRY_COMPARE(service.health(), QString("fine"));
        QString waited;
        service.call(QStringLiteral("storage"), {}, [&](bool, const QJsonObject &, const QString &c) { waited = c; });
        QTest::qWait(50);
        fake.read();
        QVERIFY(fake.asked.isEmpty());
        QVERIFY(waited.isEmpty());
        fake.send({{"event", "source"}, {"kind", "folder"}, {"name", "Pictures"}});
        QTRY_COMPARE(waited, QString("incompatible"));
        QCOMPARE(service.health(), QString("incompatible"));

        // And one of its own: what waited for its hello goes then.
        fake.peer->disconnectFromServer();
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        QTRY_COMPARE(service.health(), QString("fine"));
        bool answered = false;
        service.call(QStringLiteral("storage"), {}, [&](bool ok, const QJsonObject &, const QString &) { answered = ok; });
        QTest::qWait(50);
        fake.read();
        QVERIFY(fake.asked.isEmpty());
        fake.greet();
        QTRY_COMPARE((fake.read(), fake.asked.size()), 1);
        QCOMPARE(fake.asked.first().value("method").toString(), QString("storage"));
        fake.send({{"id", fake.asked.first().value("id")}, {"ok", true}, {"result", QJsonObject{}}});
        QTRY_VERIFY(answered);
        qunsetenv("PINHOLE_SOCKET");
    }

    void theViewersAnswerFollowsItsPhotoThroughAReload() {
        QTemporaryDir dir, after;
        makeLibrary(dir.path(), {{"a", 1000}, {"b", 2000}, {"c", 3000}});
        makeLibrary(after.path(), {{"new", 500}, {"a", 1000}, {"b", 2000}, {"c", 3000}});
        Library library, reloaded;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        QVERIFY(reloaded.open(after.path(), &error));
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(dir.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();

        Viewer viewer(&library, &service);
        viewer.setViewport(800, 600);
        viewer.openAt(2, {});
        auto asked = [&fake](const char *asset, const char *kind) {
            fake.read();
            for (const QJsonObject &line : std::as_const(fake.asked)) {
                const QJsonObject params = line.value("params").toObject();
                if (params.value("asset").toString() == QLatin1String(asset) && params.value("kind").toString() == QLatin1String(kind))
                    return line;
            }
            return QJsonObject();
        };
        QTRY_VERIFY(!asked("c", "medium").isEmpty());

        // A photo comes in before c: c is at 3 now, and 2 is b.
        library.swap(reloaded);
        viewer.libraryReloaded({1, 2, 3});
        QCOMPARE(viewer.index(), 3);
        fake.send({{"id", asked("c", "medium").value("id")}, {"ok", true},
                   {"result", QJsonObject{{"path", dir.path() + "/c-medium.jpg"}}}});
        // c's medium is shown, and the sharper one asked for is c's too.
        QTRY_VERIFY(viewer.image().toString().contains("c-medium.jpg"));
        QTRY_VERIFY(!asked("c", "full").isEmpty() || !asked("c", "original").isEmpty());
        QVERIFY(asked("b", "full").isEmpty() && asked("b", "original").isEmpty());
        qunsetenv("PINHOLE_SOCKET");
    }

    void withNoServiceToBeHadWhatWaitsIsAnsweredSoAndSoIsWhatIsAskedAfter() {
        QTemporaryDir dir;
        // Nothing listens there, and none is started (initTestCase).
        qputenv("PINHOLE_SOCKET", (dir.path() + "/none.sock").toUtf8());
        ServiceClient service(dir.path());
        QString waited;
        service.call(QStringLiteral("storage"), {}, [&](bool, const QJsonObject &, const QString &c) { waited = c; });
        QVERIFY(waited.isEmpty());
        // Some ten seconds of trying, then it gives up.
        QTRY_COMPARE_WITH_TIMEOUT(service.health(), QString("no_service"), 20000);
        QCOMPARE(waited, QString("no_service"));
        QString after;
        service.call(QStringLiteral("storage"), {}, [&](bool, const QJsonObject &, const QString &c) { after = c; });
        QVERIFY(after.isEmpty()); // after this returns, as an answer would come
        QTRY_COMPARE(after, QString("no_service"));
        qunsetenv("PINHOLE_SOCKET");
    }

    void aPluginsLibraryKeyIsTheLibrarysUntilAnothersPhotosAreIn() {
        QTemporaryDir a, b;
        makeLibrary(a.path(), {{"x.jpg", 1}}, false, "folder:/a");
        makeLibrary(b.path(), {{"x.jpg", 1}, {"y.jpg", 2}}, false, "folder:/b");
        Library library, next;
        QString error;
        QVERIFY(library.open(a.path(), &error));
        QVERIFY(next.open(b.path(), &error));
        FakeService fake;
        QVERIFY(fake.server.listen(a.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (a.path() + "/s.sock").toUtf8());
        ServiceClient service(a.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();
        PluginLibrary api(&library);
        QCOMPARE(api.key(), QString("folder:/a"));
        QCOMPARE(api.item(0).value("avgColor").toString(), QString("#000000"));
        // The service says another source is shown; its library is not
        // read yet: the key is still the photos' own.
        fake.send({{"event", "source"}, {"kind", "folder"}, {"name", "b"}, {"path", "/b"}});
        QTRY_COMPARE(service.source().value("path").toString(), QString("/b"));
        QCOMPARE(api.key(), QString("folder:/a"));
        QCOMPARE(api.count(), 1);
        // Now its photos are in, and the key with them.
        library.swap(next);
        api.reloaded();
        QCOMPARE(api.key(), QString("folder:/b"));
        QCOMPARE(api.count(), 2);
        qunsetenv("PINHOLE_SOCKET");
    }

    void aPluginsProgramsArePromisesThatSayHowTheyEnded() {
        PluginBench bench({{"t", {}}});
        QVERIFY(bench.host);
        bench.js(R"(
            const say = what => said.push(what)
            pinhole.run("sh", ["-c", "echo hi"]).then(r => say("ok " + r.stdout.trim() + " " + r.code))
            pinhole.run("sh", ["-c", "echo bad >&2; exit 3"]).then(null, e => say(e.code + " " + e.exitCode + " " + e.message))
            pinhole.run("cat", [], { input: "fed" }).then(r => say("cat " + r.stdout))
            pinhole.run("sh", ["-c", "echo $X"], { env: { X: "y" } }).then(r => say("env " + r.stdout.trim()))
            pinhole.run("/no/such/program", []).then(null, e => say(e.code))
            pinhole.run("sleep", ["5"], { timeout: 50 }).then(null, e => say(e.code))
            const p = pinhole.run("sleep", ["5"])
            p.then(null, e => say(e.code))
            p.cancel()
        )");
        QTRY_COMPARE(bench.said().size(), 7);
        QStringList said = bench.said();
        said.sort();
        QCOMPARE(said, QStringList({"cancelled", "cat fed", "env y", "exit 3 bad", "ok hi 0", "start", "timeout"}));
        // As before API 2: a callback in place of the options.
        bench.js(R"(pinhole.run("sh", ["-c", "echo old; exit 2"], (code, out) => said.push("old " + code + " " + out.trim())))");
        QTRY_COMPARE(bench.said().size(), 8);
        QCOMPARE(bench.said().last(), QString("old 2 old"));
    }

    void whatAPluginStartedEndsWithItAndAnswersNoMore() {
        PluginBench bench({{"t", {}}});
        const QString marker = bench.dir.path() + "/marker";
        bench.js(QStringLiteral(R"(
            pinhole.run("sh", ["-c", "sleep 0.5; touch %1"]).then(() => said.push("promise"), () => said.push("rejected"))
            pinhole.run("sh", ["-c", "sleep 0.5"], () => said.push("callback"))
            pinhole.fetch(0, "medium").then(() => said.push("fetched"), () => said.push("not fetched"))
        )").arg(marker));
        // Turned off: the program is stopped, and nothing it started says
        // anything to it.
        bench.plugins->setEnabled("t", false);
        QTest::qWait(1500);
        QVERIFY(!QFile::exists(marker));
        QCOMPARE(bench.said(), QStringList());
    }

    void aPluginHearsAProgramALineAtATimeAndStopsItGently() {
        PluginBench bench({{"t", {}}});
        const QString told = bench.dir.path() + "/told", alsoTold = bench.dir.path() + "/also";
        // Each line as it comes; what follows the last is the stdout.
        bench.js(R"(pinhole.run("sh", ["-c", "echo a; sleep 0.2; echo b; printf c"], { output: line => said.push("line " + line) })
                        .then(r => said.push("rest " + r.stdout)))");
        QTRY_COMPARE(bench.said(), QStringList({"line a", "line b", "rest c"}));
        // Cancelled, it is asked to end (SIGTERM) and can tidy up first.
        const QString script = bench.dir.path() + "/gently.sh";
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("trap 'touch \"$1\"; exit 0' TERM\nsleep 5 &\nwait\n");
        file.close();
        bench.js(QStringLiteral(R"(var p = pinhole.run("sh", ["%1", "%2"]); p.then(null, e => said.push(e.code)))").arg(script, told));
        QTest::qWait(300);
        bench.js("p.cancel()");
        QTRY_VERIFY(QFile::exists(told));
        QTRY_COMPARE(bench.said().last(), QString("cancelled"));
        // So too when the plugin is turned off, the program its own.
        bench.js(QStringLiteral(R"(pinhole.run("sh", ["%1", "%2"]))").arg(script, alsoTold));
        QTest::qWait(300);
        bench.plugins->setEnabled("t", false);
        QTRY_VERIFY(QFile::exists(alsoTold));
    }

    void aPluginKeepsTheScreenOnUntilItLetsGoOrEnds() {
        PluginBench bench({{"t", {}}, {"u", {}}});
        KeepAwake *awake = bench.plugins->keepAwake();
        auto *u = qobject_cast<PluginHost *>(bench.plugins->at("u").value("host").value<QObject *>());
        QVERIFY(!awake->wanted());
        // Asked for; with no Wayland window here, not held yet.
        QCOMPARE(bench.js("pinhole.keepAwake(true)").toBool(), false);
        QVERIFY(awake->wanted());
        QVERIFY(!awake->inhibiting());
        // Kept on while any plugin asks.
        u->keepAwake(true);
        bench.js("pinhole.keepAwake(false)");
        QVERIFY(awake->wanted());
        u->keepAwake(false);
        QVERIFY(!awake->wanted());
        // Turned off, it lets go.
        bench.js("pinhole.keepAwake(true)");
        bench.plugins->setEnabled("t", false);
        QVERIFY(!awake->wanted());
        // Loaded again, its file changed, it lets go.
        bench.plugins->setEnabled("t", true);
        bench.js("pinhole.keepAwake(true)");
        QVERIFY(awake->wanted());
        write(bench.dir.path() + "/plugins/t/Main.qml", "import QtQuick\nItem { objectName: \"again\" }\n");
        bench.plugins->rescan();
        QVERIFY(!awake->wanted());
        // Gone, it lets go.
        bench.js("pinhole.keepAwake(true)");
        QVERIFY(QDir(bench.dir.path() + "/plugins/t").removeRecursively());
        bench.plugins->rescan();
        QVERIFY(!awake->wanted());
    }

    // Kept on only while the window is seen: on a workspace Hyprland is not
    // showing it stays visible, but Qt has it unexposed once its frames stop
    // coming, and it lets go; seen again, it holds again.
    void theScreenIsKeptOnOnlyWhileTheWindowIsSeen() {
        // Exposed as the test says (the offscreen platform has a shown
        // window exposed for good).
        struct Faked : KeepAwake {
            bool seen = true;
            bool exposed() const override { return seen; }
        } awake;
        QWindow window;
        window.resize(100, 100);
        awake.setWindow(&window);
        awake.hold(this, true);
        QVERIFY(!awake.shows());
        window.show();
        QTRY_VERIFY(awake.shows());
        // A frame asked of it now and then, still as it is (a frame held
        // still), so Qt finds it unexposed where none comes; not once let go.
        struct Frames : QObject {
            int asked = 0;
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::UpdateRequest)
                    ++asked;
                return false;
            }
        } frames;
        window.installEventFilter(&frames);
        awake.setAskEvery(50);
        QTRY_VERIFY(frames.asked >= 3);
        awake.hold(this, false);
        QTest::qWait(100);
        const int asked = frames.asked;
        QTest::qWait(300);
        QCOMPARE(frames.asked, asked);
        awake.hold(this, true);
        QSignalSpy changed(&awake, &KeepAwake::showsChanged);
        // Its frames stop coming: still visible, no longer exposed.
        awake.seen = false;
        QExposeEvent unexposed{QRegion()};
        QCoreApplication::sendEvent(&window, &unexposed);
        QVERIFY(window.isVisible());
        QTRY_VERIFY(!awake.shows());
        QCOMPARE(changed.count(), 1);
        QVERIFY(awake.wanted() && !awake.inhibiting());
        awake.seen = true;
        QExposeEvent exposed{QRegion(0, 0, 100, 100)};
        QCoreApplication::sendEvent(&window, &exposed);
        QTRY_VERIFY(awake.shows());
        QCOMPARE(changed.count(), 2);
        window.hide();
        QTRY_VERIFY(!awake.shows());
    }

    // The window closing: no event loop is left to kill a program a second
    // after asking it to end, so the host waits that second itself.
    void aProgramThatWillNotEndGoesWithTheWindow() {
        auto bench = std::make_unique<PluginBench>(QList<std::pair<QString, QJsonObject>>{{"t", {}}});
        const QString pidFile = bench->dir.path() + "/pid";
        // Deaf to SIGTERM, as is what it runs in its place.
        bench->js(QStringLiteral(R"(pinhole.run("sh", ["-c", "trap '' TERM; echo $$ > %1; exec sleep 30"]))").arg(pidFile));
        QTRY_VERIFY(QFileInfo(pidFile).size() > 0);
        QFile file(pidFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const pid_t pid = file.readAll().trimmed().toInt();
        QVERIFY(pid > 0);
        QVERIFY(::kill(pid, 0) == 0);
        QElapsedTimer clock;
        clock.start();
        bench->plugins.reset();
        // Gone, and reaped, before anything else runs.
        QVERIFY(::kill(pid, 0) != 0);
        QVERIFY(clock.elapsed() >= 900);
        QVERIFY(clock.elapsed() < 3000);
    }

    // Two plugins' programs deaf to SIGTERM: a second together, not one
    // each, before the window is gone.
    void programsThatWillNotEndGoTogetherWithTheWindow() {
        auto bench = std::make_unique<PluginBench>(QList<std::pair<QString, QJsonObject>>{{"t", {}}, {"u", {}}});
        auto *u = qobject_cast<PluginHost *>(bench->plugins->at("u").value("host").value<QObject *>());
        QList<pid_t> pids;
        for (PluginHost *host : {bench->host, u}) {
            const QString pidFile = bench->dir.path() + "/pid-" + host->property("id").toString() + QString::number(pids.size());
            bench->engine.globalObject().setProperty("host", bench->engine.newQObject(host));
            bench->js(QStringLiteral(R"(host.run("sh", ["-c", "trap '' TERM; echo $$ > %1; exec sleep 30"]))").arg(pidFile));
            QTRY_VERIFY(QFileInfo(pidFile).size() > 0);
            QFile file(pidFile);
            QVERIFY(file.open(QIODevice::ReadOnly));
            pids << file.readAll().trimmed().toInt();
            QVERIFY(pids.last() > 0 && ::kill(pids.last(), 0) == 0);
        }
        QElapsedTimer clock;
        clock.start();
        bench->plugins.reset();
        for (pid_t pid : pids)
            QVERIFY(::kill(pid, 0) != 0);
        QVERIFY(clock.elapsed() >= 900);
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
    }

private:
    // A panel the Scroller scrolls (SidePanel.qml), with a list of its own
    // in it, as a plugin's settings may have, and a switch: shown, and the
    // wheel and the touchpad given to it as the window gets them.
    struct ScrollerBench {
        QQmlEngine engine;
        std::unique_ptr<QObject> made;
        QQuickWindow *window = nullptr;
        QQuickItem *panel = nullptr, *list = nullptr, *toggle = nullptr;
        quint64 time = 1000000;
        explicit ScrollerBench() {
            QQmlComponent component(&engine);
            component.setData(R"(import QtQuick
                import Pinhole
                Window {
                    width: 400; height: 400
                    property alias panel: panel
                    property alias list: list
                    property alias toggle: toggle
                    property int beneath: 0
                    MouseArea { anchors.fill: parent; onWheel: wheel => beneath++; onClicked: beneath++ }
                    Flickable {
                        id: panel
                        anchors.fill: parent
                        contentHeight: column.height
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true
                        Column {
                            id: column
                            width: 400
                            Item { width: 400; height: 100 }
                            Rectangle {
                                id: toggle
                                property int clicks: 0
                                width: 100; height: 40
                                MouseArea { anchors.fill: parent; onClicked: toggle.clicks++ }
                            }
                            Item { width: 400; height: 60 }
                            Item {
                                width: 400; height: 120
                                ListView {
                                    id: list
                                    anchors.fill: parent
                                    clip: true
                                    boundsBehavior: Flickable.StopAtBounds
                                    model: 30
                                    delegate: Rectangle { width: 400; height: 20 }
                                }
                                Scroller { anchors.fill: parent; flickable: list }
                            }
                            Item { width: 400; height: 3000 }
                        }
                    }
                    Scroller { anchors.fill: parent; flickable: panel }
                })", QUrl());
            made.reset(component.create());
            if (!made)
                qWarning() << component.errorString();
            window = qobject_cast<QQuickWindow *>(made.get());
            if (!window)
                return;
            window->show();
            panel = window->property("panel").value<QQuickItem *>();
            list = window->property("list").value<QQuickItem *>();
            toggle = window->property("toggle").value<QQuickItem *>();
        }
        double y(QQuickItem *f) const { return f->property("contentY").toDouble(); }
        QPointF centre(QQuickItem *item) const { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }
        // A swipe's phases come from a touchpad, a notch from the mouse.
        static const QPointingDevice *touchpad() {
            static QPointingDevice *device = [] {
                auto *d = new QPointingDevice(QStringLiteral("test touchpad"), 4713, QInputDevice::DeviceType::TouchPad,
                                              QPointingDevice::PointerType::Finger,
                                              QInputDevice::Capability::Position | QInputDevice::Capability::Scroll
                                                  | QInputDevice::Capability::PixelScroll,
                                              5, 0);
                QWindowSystemInterface::registerInputDevice(d);
                return d;
            }();
            return device;
        }
        void wheel(QPointF at, QPoint pixels, QPoint angle, Qt::ScrollPhase phase = Qt::NoScrollPhase) {
            QWheelEvent event(at, window->mapToGlobal(at), pixels, angle, Qt::NoButton, Qt::NoModifier, phase, false,
                              Qt::MouseEventNotSynthesized,
                              phase != Qt::NoScrollPhase ? touchpad() : QPointingDevice::primaryPointingDevice());
            event.setTimestamp(time += 8);
            QGuiApplication::sendEvent(window, &event);
        }
        // Two fingers: a begin, `steps` of `by`, an end.
        void swipe(QPointF at, QPoint by, int steps) {
            time += 1000;
            wheel(at, {}, {}, Qt::ScrollBegin);
            for (int i = 0; i < steps; ++i)
                wheel(at, by, by * 8, Qt::ScrollUpdate);
            wheel(at, {}, {}, Qt::ScrollEnd);
        }
        void click(QPointF at) {
            for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, at, window->mapToGlobal(at), Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                event.setTimestamp(time += 8);
                QGuiApplication::sendEvent(window, &event);
            }
        }
    };

private slots:
    // A list within the panel, a Scroller of its own over it, that can still
    // go the way the wheel goes has it first, a notch and a swipe alike; at
    // its end, the panel; back from its end, the list again. Nothing
    // reaches what is beneath.
    void aListInAPanelScrollsFirst() {
        ScrollerBench bench;
        QVERIFY(bench.window);
        QVERIFY(QTest::qWaitForWindowExposed(bench.window));
        const QPointF overList = bench.centre(bench.list);
        // A notch down: the list, not the panel.
        bench.wheel(overList, {0, -15}, {0, -120});
        QTRY_VERIFY(bench.y(bench.list) > 5);
        QTest::qWait(300);
        QCOMPARE(bench.y(bench.panel), 0.0);
        // A swipe down: the list again, by the fingers.
        bench.list->setProperty("contentY", 0);
        bench.swipe(overList, {0, -6}, 6);
        QTRY_VERIFY(bench.y(bench.list) > 20);
        QTest::qWait(300);
        QCOMPARE(bench.y(bench.panel), 0.0);
        // At its end: the panel takes them.
        const double end = bench.list->property("contentHeight").toDouble() - bench.list->height();
        bench.list->setProperty("contentY", end);
        bench.wheel(overList, {0, -15}, {0, -120});
        QTRY_VERIFY(bench.y(bench.panel) > 5);
        QCOMPARE(bench.y(bench.list), end);
        // A swipe from there too, the panel following the fingers.
        const double panelWas = bench.y(bench.panel);
        bench.swipe(bench.centre(bench.list), {0, -6}, 6);
        QTRY_VERIFY(bench.y(bench.panel) > panelWas + 20);
        QCOMPARE(bench.y(bench.list), end);
        // Back up over it, still at its end, in view again: the list.
        QTest::qWait(1500);
        bench.panel->setProperty("contentY", 100);
        const double panelNow = bench.y(bench.panel);
        bench.wheel(bench.centre(bench.list), {0, 15}, {0, 120});
        QTRY_VERIFY(bench.y(bench.list) < end - 5);
        QTest::qWait(300);
        QCOMPARE(bench.y(bench.panel), panelNow);
        QCOMPARE(bench.window->property("beneath").toInt(), 0);
    }

    // A click on a switch while the panel coasts stops it there, and does
    // nothing else: the switch is not pressed (with iCloud's, a slip would
    // turn a source off). At rest, the next click presses it.
    // A plugin's strip (a ScrollList across) goes across by a sideways
    // swipe, and by the wheel itself, which has nowhere else to go; the
    // sideways wheel too. Nothing reaches what is beneath. A Panel with
    // steps steps a notch at a time, and once a swipe.
    void aStripScrollsAcrossAndAPanelSteps() {
        Plugins::registerTypes();
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(import QtQuick
            import Pinhole.Plugin
            Window {
                width: 400; height: 300
                property int beneath: 0
                property int stepCount: 0
                property alias strip: strip
                MouseArea { anchors.fill: parent; onWheel: wheel => beneath++ }
                Panel {
                    anchors.fill: undefined
                    y: 200; width: 400; height: 100
                    color: "black"
                    steps: Qt.Horizontal | Qt.Vertical
                    onStepped: step => stepCount += step
                }
                ScrollList {
                    id: strip
                    width: 400; height: 100
                    orientation: ListView.Horizontal
                    model: 40
                    delegate: Rectangle { width: 50; height: 100 }
                }
            })", QUrl());
        std::unique_ptr<QObject> made(component.create());
        QVERIFY2(made, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(made.get());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *strip = window->property("strip").value<QQuickItem *>();
        quint64 time = 2000000;
        auto wheel = [&](QPointF at, QPoint pixels, QPoint angle, Qt::ScrollPhase phase) {
            QWheelEvent event(at, window->mapToGlobal(at), pixels, angle, Qt::NoButton, Qt::NoModifier, phase, false,
                              Qt::MouseEventNotSynthesized,
                              phase != Qt::NoScrollPhase ? ScrollerBench::touchpad() : QPointingDevice::primaryPointingDevice());
            event.setTimestamp(time += 8);
            QGuiApplication::sendEvent(window, &event);
        };
        const QPointF over(200, 50), panel(200, 250);
        // A notch of the wheel: across, 180 px as the grid's.
        wheel(over, {0, -15}, {0, -120}, Qt::NoScrollPhase);
        QTRY_VERIFY(strip->property("contentX").toDouble() > 170);
        QTest::qWait(400);
        QCOMPARE(strip->property("contentY").toDouble(), 0.0);
        // A sideways swipe, back.
        const double was = strip->property("contentX").toDouble();
        time += 1000;
        wheel(over, {}, {}, Qt::ScrollBegin);
        for (int i = 0; i < 6; ++i)
            wheel(over, {10, 0}, {80, 0}, Qt::ScrollUpdate);
        wheel(over, {}, {}, Qt::ScrollEnd);
        QTRY_VERIFY(strip->property("contentX").toDouble() < was - 40);
        // The Panel: two notches, two steps; a long swipe, one.
        wheel(panel, {0, -15}, {0, -120}, Qt::NoScrollPhase);
        wheel(panel, {0, -15}, {0, -120}, Qt::NoScrollPhase);
        QCOMPARE(window->property("stepCount").toInt(), 2);
        time += 1000;
        wheel(panel, {}, {}, Qt::ScrollBegin);
        for (int i = 0; i < 20; ++i)
            wheel(panel, {-20, 0}, {-160, 0}, Qt::ScrollUpdate);
        wheel(panel, {}, {}, Qt::ScrollEnd);
        QCOMPARE(window->property("stepCount").toInt(), 3);
        QCOMPARE(window->property("beneath").toInt(), 0);
    }

    // `pinhole plugin check` refuses the footguns, each with what to use
    // instead, and warns of a setting written often, a key as Qt names
    // it, words shown untranslated and words with no Polish.
    void checkRefusesTheFootguns() {
        QTemporaryDir dir;
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
// A ListView { in a comment is none.
PinholePlugin {
    id: plugin
    ListView { }
    Flickable { }
    PinchHandler { }
    MouseArea { onWheel: wheel => {} }
    Timer { onTriggered: pinhole.setSetting("seen", Date.now()) }
    Component.onCompleted: {
        pinhole.run("sqlite3", [pinhole.library.dir + "/library.sqlite", "SELECT 1"])
        pinhole.run("wl-copy", ["x"])
        const where = "~/Pictures/Mine"
        pinhole.notify(qsTr("Press Shift+S to save"))
    }
    property string label: "Not translated"
    property var view: ListView.view
}
)");
        const Plugins::Findings found = Plugins::footguns(dir.path());
        const QString errors = found.errors.join('\n'), warnings = found.warnings.join('\n');
        QVERIFY2(errors.contains("Main.qml:6: a ListView") && errors.contains("use ScrollList"), qPrintable(errors));
        QVERIFY2(errors.contains("Main.qml:7: a Flickable") && errors.contains("use ScrollArea"), qPrintable(errors));
        QVERIFY2(errors.contains("Main.qml:8: a PinchHandler") && errors.contains("use PinchZoom"), qPrintable(errors));
        QVERIFY2(errors.contains("Main.qml:9: a MouseArea's onWheel"), qPrintable(errors));
        QVERIFY2(errors.contains("Main.qml:12: sqlite3") && errors.contains("pinhole.query"), qPrintable(errors));
        QVERIFY2(errors.contains("Main.qml:13: wl-copy") && errors.contains("pinhole.copyFile"), qPrintable(errors));
        QVERIFY2(errors.contains("Main.qml:14: ~/Pictures") && errors.contains("pinhole.picturePath"), qPrintable(errors));
        QCOMPARE(found.errors.size(), 7);
        QVERIFY2(warnings.contains("Main.qml:10: a setting written in onTriggered"), qPrintable(warnings));
        QVERIFY2(warnings.contains("\"Shift+S\" names a key"), qPrintable(warnings));
        QVERIFY2(warnings.contains("i18n/pl.json: there is none"), qPrintable(warnings));
        // The plugin `pinhole plugin new` makes passes, warning of nothing.
        write(dir.path() + "/Main.qml", "import QtQuick\nimport Pinhole.Plugin\nPinholePlugin {\n"
                                        "    settings: Component { ScrollList { model: 3; delegate: UiText { text: qsTr(\"Row\") } } }\n}\n");
        QDir().mkpath(dir.path() + "/i18n");
        write(dir.path() + "/i18n/pl.json", "{ \"Row\": \"Wiersz\" }\n");
        const Plugins::Findings clean = Plugins::footguns(dir.path());
        QVERIFY2(clean.errors.isEmpty() && clean.warnings.isEmpty(), qPrintable((clean.errors + clean.warnings).join('\n')));
    }

    // A reviewer's careless ways round check, each refused; and words
    // shown that only name a program or Pictures are not.
    void checkRefusesTheEvasions() {
        QTemporaryDir dir;
        write(dir.path() + "/Main.qml", R"qml(import QtQuick
import QtQuick.Controls as C
import QtMultimedia
import Pinhole.Plugin
PinholePlugin {
    id: plugin
    Component.onCompleted: {
        Qt.createQmlObject("import QtQuick; Flickable { anchors.fill: parent }", plugin)
        const prog = "sqlite" + "3"
        pinhole.run("/usr/bin/sqlite3", ["x"])
        pinhole.run("sh", ["-c", "sqlite3 x .tables"])
        pinhole.run(`sqlite3`, ["x"])
        pinhole.run("sh", ["-c", "wl-copy < f"])
        const pics = pinhole.library.dir.split("/").slice(0, 3).join("/") + "/Pictures/Pinhole/x.png"
        pinhole.run("sh", ["-c", "cp f $(xdg-user-dir PICTURES)"])
    }
    TableView { anchors.fill: parent }
    PathView { model: 10 }
    C.Tumbler { model: 10 }
    MultiPointTouchArea { anchors.fill: parent }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }
    Item {
        anchors.fill: parent
        MouseArea { anchors.fill: parent }
    }
    Panel { MouseArea { anchors.fill: parent } }
    VideoOutput { }
    Video { }
    help: [["F9", "Untranslated help"], ["F10", qsTr("Translated")]]
}
)qml");
        const Plugins::Findings found = Plugins::footguns(dir.path());
        const QString errors = found.errors.join('\n'), warnings = found.warnings.join('\n');
        for (const char *want : {"Main.qml:8: Qt.createQmlObject", "Main.qml:9: sqlite3", "Main.qml:10: sqlite3",
                                 "Main.qml:11: sqlite3", "Main.qml:12: sqlite3", "Main.qml:13: wl-copy",
                                 "Main.qml:14: /Pictures by hand", "Main.qml:15: Pictures found by hand",
                                 "Main.qml:17: a TableView", "Main.qml:18: a PathView", "Main.qml:19: a Tumbler",
                                 "Main.qml:20: a MultiPointTouchArea", "Main.qml:21: a MouseArea over the whole window",
                                 "Main.qml:24: a MouseArea over the whole window", "Main.qml:27: a VideoOutput",
                                 "Main.qml:28: a Video shows"})
            QVERIFY2(errors.contains(QLatin1String(want)), qPrintable(QStringLiteral("%1 not in:\n%2").arg(want, errors)));
        // The MouseArea in a Panel is the Panel's.
        QVERIFY2(!errors.contains("Main.qml:26:"), qPrintable(errors));
        QCOMPARE(found.errors.size(), 16);
        QVERIFY2(warnings.contains("Main.qml:29: \"Untranslated help\" is shown as it is"), qPrintable(warnings));
        QVERIFY2(!warnings.contains("Translated\" is shown"), qPrintable(warnings));

        // Words shown that name them: no program run, no path built.
        write(dir.path() + "/Main.qml", R"(import QtQuick
import Pinhole.Plugin
PinholePlugin {
    actions: [PluginAction { place: "grid"; text: qsTr("Kept in ~/Pictures/Pinhole"); key: "F9"
                             onTriggered: pinhole.notify(qsTr("Needs sqlite3 no more")) }]
}
)");
        QDir().mkpath(dir.path() + "/i18n");
        write(dir.path() + "/i18n/pl.json", "{ \"Kept in ~/Pictures/Pinhole\": \"W ~/Obrazy/Pinhole\", "
                                            "\"Needs sqlite3 no more\": \"Już bez sqlite3\" }\n");
        const Plugins::Findings words = Plugins::footguns(dir.path());
        QVERIFY2(words.errors.isEmpty() && words.warnings.isEmpty(), qPrintable((words.errors + words.warnings).join('\n')));
    }

    // pinhole.run refuses what check does, however its name was made.
    void runRefusesWhatCheckDoes() {
        QVERIFY(Plugins::refusedRun("sqlite3", {"x"}).contains("pinhole.query"));
        QVERIFY(Plugins::refusedRun(QStringLiteral("sqlite") + "3", {}).contains("pinhole.query"));
        QVERIFY(Plugins::refusedRun("/usr/bin/sqlite3", {}).contains("pinhole.query"));
        QVERIFY(Plugins::refusedRun("sh", {"-c", "sqlite3 x .tables"}).contains("pinhole.query"));
        QVERIFY(Plugins::refusedRun("bash", {"-c", "echo `sqlite3 x`"}).contains("pinhole.query"));
        QVERIFY(Plugins::refusedRun("env", {"A=1", "/usr/bin/sqlite3", "x"}).contains("pinhole.query"));
        QVERIFY(Plugins::refusedRun("sh", {"-c", "wl-copy < f"}).contains("wl-copy"));
        QVERIFY(Plugins::refusedRun("xclip", {"-i"}).contains("pinhole.copyFile"));
        QVERIFY(Plugins::refusedRun("sh", {"-c", "cp f \"$(xdg-user-dir PICTURES)\""}).contains("pinhole.picturePath"));
        // Its own programs, and the names inside other words, run.
        QVERIFY(Plugins::refusedRun("magick", {"in.png", "out.png"}).isEmpty());
        QVERIFY(Plugins::refusedRun("sh", {"-c", "cat > \"$1\"", "sh", "/tmp/x.txt"}).isEmpty());
        QVERIFY(Plugins::refusedRun("sh", {"-c", "ls /usr/lib/libsqlite3.so"}).isEmpty());
    }

    void aClickThatStopsAPanelPressesNothing() {
        ScrollerBench bench;
        QVERIFY(bench.window);
        QVERIFY(QTest::qWaitForWindowExposed(bench.window));
        auto *toggle = bench.toggle;
        // Clicked at rest: pressed.
        bench.click(bench.centre(toggle));
        QCOMPARE(toggle->property("clicks").toInt(), 1);
        // Flung gently beside the list, so the switch stays in view as it
        // coasts.
        bench.swipe(QPointF(300, 50), {0, -3}, 4);
        QTest::qWait(40);
        const double coasting = bench.y(bench.panel);
        QTest::qWait(40);
        QVERIFY2(bench.y(bench.panel) > coasting, "the panel coasts");
        const QPointF at = bench.centre(toggle);
        QVERIFY(at.y() > 0 && at.y() < bench.window->height());
        bench.click(at);
        const double stopped = bench.y(bench.panel);
        QCOMPARE(toggle->property("clicks").toInt(), 1);
        QTest::qWait(200);
        QCOMPARE(bench.y(bench.panel), stopped);
        // At rest again: pressed.
        bench.click(bench.centre(toggle));
        QCOMPARE(toggle->property("clicks").toInt(), 2);
        QCOMPARE(bench.window->property("beneath").toInt(), 0);
    }

    // A field that took the keys and was then hidden or turned off: Pinhole's
    // keys, each an application shortcut a field takes first, reach Pinhole
    // again, and nothing is typed into it.
    void aHiddenFieldGivesBackTheKeys() {
        Keymap keymap;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(import QtQuick
            Window {
                width: 400; height: 300
                property int o: 0
                property int right: 0
                property alias field: field
                property alias panel: panel
                Item { objectName: "grid"; anchors.fill: parent; focus: true }
                Shortcut { sequences: ["O"]; context: Qt.ApplicationShortcut; onActivated: o++ }
                Shortcut { sequences: ["Right"]; context: Qt.ApplicationShortcut; onActivated: right++ }
                Item { id: panel; anchors.fill: parent; TextInput { id: field; width: 100; height: 20 } }
            })", QUrl());
        std::unique_ptr<QObject> made(component.create());
        QVERIFY2(made, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(made.get());
        window->show();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        auto *field = window->property("field").value<QQuickItem *>();
        auto *panel = window->property("panel").value<QQuickItem *>();
        QTest::keyClick(window, Qt::Key_O);
        QCOMPARE(window->property("o").toInt(), 1);
        field->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_1);
        QCOMPARE(field->property("text").toString(), QString("1"));
        panel->setVisible(false);
        QTest::keyClick(window, Qt::Key_O);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(window->property("o").toInt(), 2);
        QCOMPARE(window->property("right").toInt(), 1);
        QCOMPARE(field->property("text").toString(), QString("1"));
        QVERIFY(!field->hasActiveFocus());
        QVERIFY(window->findChild<QQuickItem *>("grid")->hasActiveFocus());
        // Turned off, the same.
        panel->setVisible(true);
        field->forceActiveFocus();
        field->setEnabled(false);
        QTest::keyClick(window, Qt::Key_O);
        QCOMPARE(window->property("o").toInt(), 3);
        QCOMPARE(field->property("text").toString(), QString("1"));
    }

    // Send to phone's PIN: its field has the keys while the panel shows and
    // asks for one, and never once it is closed, or asked while closed;
    // opened again on the same photos, it still asks.
    void sendToPhonesPinLetsGoOfTheKeys() {
        PluginBench bench({{"pinhole.send", {}}});
        // No pinhole-send beside the bench's plugin: opening it finds no
        // device, and nothing goes on any network.
        QVERIFY(!QFile::exists(bench.host->property("dir").toString() + "/sender/target/release/pinhole-send"));
        QVERIFY(!QFile::exists(bench.host->property("dir").toString() + "/pinhole-send"));
        Keymap keymap;
        Theme theme(bench.dir.path());
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        bench.engine.rootContext()->setContextProperty("theme", &theme);
        Plugins::registerTypes();
        QQmlComponent shell(&bench.engine);
        shell.setData(R"(import QtQuick
            Window {
                width: 800; height: 600
                property int o: 0
                property int right: 0
                Item { objectName: "grid"; anchors.fill: parent; focus: true }
                Shortcut { sequences: ["O"]; context: Qt.ApplicationShortcut; onActivated: o++ }
                Shortcut { sequences: ["Right"]; context: Qt.ApplicationShortcut; onActivated: right++ }
            })", QUrl());
        std::unique_ptr<QObject> made(shell.create());
        QVERIFY2(made, qPrintable(shell.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(made.get());
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/send/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.send&load=0");
        std::unique_ptr<QObject> plugin(
            component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(bench.host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        auto *item = qobject_cast<QQuickItem *>(plugin.get());
        item->setParentItem(window->contentItem());
        item->setSize(window->size());
        window->show();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        auto *panel = item->findChild<QQuickItem *>("sendPanel");
        auto *field = item->findChild<QQuickItem *>("send.pin");
        QVERIFY(panel && field);
        auto count = [window](const char *name) { return window->property(name).toInt(); };

        // Asked for while it shows: typed into.
        QMetaObject::invokeMethod(panel, "open", Q_ARG(QVariant, QVariantList{0}));
        QVERIFY(panel->property("shown").toBool());
        panel->setProperty("phase", "pin");
        QTRY_VERIFY(field->hasActiveFocus() && field->isVisible());
        QTest::keyClick(window, Qt::Key_1);
        QTest::keyClick(window, Qt::Key_2);
        QCOMPARE(field->property("text").toString(), QString("12"));
        QCOMPARE(count("o"), 0);
        // Closed: the keys are Pinhole's again, at once.
        QMetaObject::invokeMethod(panel, "close");
        QVERIFY(!field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_O);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(count("o"), 1);
        QCOMPARE(count("right"), 1);
        QCOMPARE(field->property("text").toString(), QString("12"));
        // Opened again on the same photo: still asked, typed into again.
        QMetaObject::invokeMethod(panel, "open", Q_ARG(QVariant, QVariantList{0}));
        QCOMPARE(panel->property("phase").toString(), QString("pin"));
        QTRY_VERIFY(field->hasActiveFocus() && field->isVisible());
        QMetaObject::invokeMethod(panel, "close");

        // Asked for while closed (a send that went on): the field never
        // takes the keys.
        panel->setProperty("phase", "");
        panel->setProperty("phase", "wrongpin");
        QTest::qWait(50);
        QVERIFY(!field->hasActiveFocus());
        QVERIFY(!field->isEnabled());
        QTest::keyClick(window, Qt::Key_O);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(count("o"), 2);
        QCOMPARE(count("right"), 2);
        // Opened, on another photo now: still asked, for that one.
        QMetaObject::invokeMethod(panel, "open", Q_ARG(QVariant, QVariantList{1}));
        QCOMPARE(panel->property("phase").toString(), QString("wrongpin"));
        QVariant chosen = panel->property("chosen");
        if (chosen.canConvert<QJSValue>())
            chosen = chosen.value<QJSValue>().toVariant();
        QCOMPARE(chosen.toList().value(0).toMap().value("id").toString(), QString("b"));
        QTRY_VERIFY(field->hasActiveFocus() && field->isVisible());
        // Its prompt gone (another device, J), the keys go back at once.
        panel->setProperty("phase", "");
        QVERIFY(!field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(count("right"), 3);
        QMetaObject::invokeMethod(panel, "close");
    }

    void whatAPluginThrowsOrWarnsOfIsItsProblemUntilItLoadsAgain() {
        PluginBench bench({{"t", {}}, {"u", {}}});
        bench.js(R"(pinhole.run("true", [], () => { throw new Error("boom") }))");
        QTRY_VERIFY(bench.host->problem().contains("boom"));
        QTRY_VERIFY(bench.plugins->at("t").value("problem").toString().contains("boom"));
        QCOMPARE(bench.plugins->at("u").value("problem").toString(), QString());
        // What the engine warns of in one of its files.
        QQmlError warning;
        warning.setUrl(QUrl::fromLocalFile(bench.dir.path() + "/plugins/u/sub/Editor.qml"));
        warning.setLine(7);
        warning.setDescription("TypeError: nope");
        emit bench.engine.warnings({warning});
        QTRY_COMPARE(bench.plugins->at("u").value("problem").toString(), QString("sub/Editor.qml:7: TypeError: nope"));
        // What a promise's chain throws, ended with .catch(pinhole.warn).
        bench.js(R"(Promise.resolve().then(() => { throw new Error("in a then") }).catch(pinhole.warn))");
        QTRY_VERIFY(bench.host->problem().contains("in a then"));
        // Loaded again (turned off): forgotten.
        bench.plugins->setEnabled("t", false);
        QTRY_COMPARE(bench.plugins->at("t").value("problem").toString(), QString());
    }

    void aPluginsSettingsAreLivePropertiesKeptAtOnce() {
        PluginBench bench({{"t", {}}});
        auto *settings = qobject_cast<QQmlPropertyMap *>(bench.host->settings());
        QVERIFY(settings);
        bench.js(R"(
            pinhole.settings.size = 3
            pinhole.settings["edits:folder:/x"] = { manual: [["a", "b"]], apart: [] }
        )");
        QCOMPARE(bench.host->setting("size").toInt(), 3);
        QCOMPARE(bench.host->setting("edits:folder:/x").toMap().value("manual").toList().first().toStringList(),
                 QStringList({"a", "b"}));
        QFile file(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll())["settings"]["t"]["size"].toInt(), 3);
        // setSetting is the same thing, and what is bound to it follows.
        bench.engine.rootContext()->setContextProperty("pinhole", bench.host);
        QQmlComponent component(&bench.engine);
        component.setData("import QtQml\nQtObject { property int size: pinhole.settings.size }", QUrl());
        std::unique_ptr<QObject> bound(component.create());
        QVERIFY2(bound, qPrintable(component.errorString()));
        QCOMPARE(bound->property("size").toInt(), 3);
        QVERIFY(bench.host->setSetting("size", 4));
        QCOMPARE(bound->property("size").toInt(), 4);
        QCOMPARE(bench.js("pinhole.settings.size").toInt(), 4);
        // Null takes one out: it reads as one never set, keys() leaves it
        // out, and so does plugins.json.
        QCOMPARE(bench.js("pinhole.settings.keys().sort().join(' ')").toString(), QString("edits:folder:/x size"));
        bench.js("pinhole.settings.size = null");
        QVERIFY(bench.js("pinhole.settings.size === undefined").toBool());
        QCOMPARE(bench.js("pinhole.settings.keys().join(' ')").toString(), QString("edits:folder:/x"));
        QVERIFY(!QJsonDocument::fromJson(read(file.fileName()).toUtf8())["settings"]["t"].toObject().contains("size"));
        bench.js("pinhole.settings.size = 5");
        QCOMPARE(bench.js("pinhole.settings.keys().sort().join(' ')").toString(), QString("edits:folder:/x size"));
    }

    // A plugin's state and cache: properties as its settings are, kept
    // apart from them and from each other, each in a file of its own,
    // written soon after (never in the middle of what changed them, nor in
    // the way of a frame), in one step; plugins.json never.
    void aPluginsStateAndCacheAreKeptApartFromItsSettings() {
        PluginBench bench({{"t", {}}, {"u", {}}});
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json";
        bench.js(R"(
            pinhole.state["edits:folder:/x"] = { manual: [["a", "b"]], apart: [] }
            pinhole.state.shown = true
            pinhole.cache["found:folder:/x"] = ["a", "c"]
            said.push(JSON.stringify(pinhole.state["edits:folder:/x"].manual), String(pinhole.cache["found:folder:/x"]))
        )");
        // There at once.
        QCOMPARE(bench.said(), (QStringList{R"([["a","b"]])", "a,c"}));
        const QString state = Plugins::stateRoot() + "/t/state.json", cache = Plugins::cacheRoot() + "/t/cache.json";
        QCOMPARE(bench.plugins->stateStore()->path("t"), state);
        QCOMPARE(bench.plugins->cacheStore()->path("t"), cache);
        // Written after this turn, not in it.
        QVERIFY(!QFileInfo::exists(state));
        QVERIFY(!QFileInfo::exists(cache));
        QTRY_VERIFY(QFileInfo::exists(state) && QFileInfo::exists(cache));
        bench.plugins->flush();
        const auto json = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        };
        QCOMPARE(json(state), (QJsonObject{{"edits:folder:/x", QJsonObject{{"manual", QJsonArray{QJsonArray{"a", "b"}}}, {"apart", QJsonArray{}}}},
                                           {"shown", true}}));
        QCOMPARE(json(cache), (QJsonObject{{"found:folder:/x", QJsonArray{"a", "c"}}}));
        // Nothing beside them left half written; and no settings at all.
        QCOMPARE(QDir(Plugins::stateRoot() + "/t").entryList(QDir::Files), QStringList{"state.json"});
        QVERIFY(!QFileInfo::exists(config));
        // Another plugin's are its own.
        QVERIFY(bench.js("pinhole.state.shown").isBool());
        auto *u = qobject_cast<PluginHost *>(bench.plugins->at("u").value("host").value<QObject *>());
        QVERIFY(!u->state()->property("shown").isValid());

        // Bound to, as settings are; null takes one out, and it reads as
        // one never set.
        bench.engine.rootContext()->setContextProperty("pinhole", bench.host);
        QQmlComponent component(&bench.engine);
        component.setData("import QtQml\nQtObject { property bool shown: pinhole.state.shown === true }", QUrl());
        std::unique_ptr<QObject> bound(component.create());
        QVERIFY2(bound, qPrintable(component.errorString()));
        QVERIFY(bound->property("shown").toBool());
        QCOMPARE(bench.js("pinhole.state.keys().sort().join(' ')").toString(), QString("edits:folder:/x shown"));
        bench.js("pinhole.state.shown = null");
        QVERIFY(!bound->property("shown").toBool());
        QVERIFY(bench.js("pinhole.state.shown").isUndefined());
        // … and keys() leaves it out, as the file does (below).
        QCOMPARE(bench.js("pinhole.state.keys().join(' ')").toString(), QString("edits:folder:/x"));
        bench.js("pinhole.cache.gone = 1; pinhole.cache.gone = null");
        QCOMPARE(bench.js("pinhole.cache.keys().join(' ')").toString(), QString("found:folder:/x"));
        // The same value again writes nothing.
        bench.plugins->flush();
        const QDateTime written = QFileInfo(cache).lastModified();
        QTest::qWait(20);
        bench.js(R"(pinhole.cache["found:folder:/x"] = ["a", "c"])");
        bench.plugins->flush();
        QCOMPARE(QFileInfo(cache).lastModified(), written);
        QVERIFY(!json(state).contains("shown"));

        // Many changes in one turn: one write, of the last.
        bench.js(R"(for (let i = 0; i < 100; ++i) pinhole.cache.n = i)");
        bench.plugins->flush();
        QCOMPARE(json(cache).value("n").toInt(), 99);

        // Read again by the next Pinhole, as it loads.
        bound.reset();
        bench.plugins.reset();
        Plugins again(&bench.library, bench.service.get(), bench.viewer.get(), bench.translator.get(), &bench.history);
        again.setEngine(&bench.engine);
        auto *host = qobject_cast<PluginHost *>(again.at("t").value("host").value<QObject *>());
        QCOMPARE(host->cache()->property("n").toInt(), 99);
        QCOMPARE(qvariant_cast<QVariantMap>(host->state()->property("edits:folder:/x")).value("manual").toList().size(), 1);
        QVERIFY(!QFileInfo::exists(config));

        // A file that cannot be written is said under its plugin.
        again.flush();
        QVERIFY(QFile::remove(state));
        QVERIFY(QDir().mkpath(state));
        bench.engine.globalObject().setProperty("other", bench.engine.newQObject(host));
        bench.js("other.state.y = 3");
        QTRY_VERIFY(host->problem().contains("state"));
        QVERIFY(QDir(state).removeRecursively());
    }

    // What Pinhole's own plugins kept in plugins.json that is not a choice
    // of the user's moves out of it as Pinhole starts: Screenshots' lists
    // to its cache, Series' edits, Info's panel and Send's devices to their
    // state, the tours seen to toured.json; plugins.json written once,
    // after them, with every switch and setting as it was; and never again.
    void whatIsNotASettingMovesOutOfPluginsJsonOnce() {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json";
        const QJsonObject edits{{"manual", QJsonArray{QJsonArray{"a", "b"}}}, {"apart", QJsonArray{"c"}}};
        const QJsonObject before{
            {"enabled", QJsonObject{{"pinhole.frame", false}, {"me.mine", true}}},
            {"settings", QJsonObject{
                {"pinhole.screenshots", QJsonObject{{"hide", true}, {"found:icloud:me@example.com", QJsonArray{"s1", "s2"}},
                                                    {"found:folder:/x", QJsonArray{"s3"}}}},
                {"pinhole.series", QJsonObject{{"recipe", QJsonObject{{"look", "warm"}}}, {"edits:icloud:me@example.com", edits},
                                               {"edits", QJsonObject{{"manual", QJsonArray{}}, {"apart", QJsonArray{"d"}}}}}},
                {"pinhole.info", QJsonObject{{"shown", true}}},
                {"pinhole.send", QJsonObject{{"compatible", true}, {"devices", QJsonArray{QJsonObject{{"fingerprint", "f"}}}}}},
                {"pinhole.frame", QJsonObject{{"seconds", 5}}},
                // Another plugin's keys of the same names are its own.
                {"me.mine", QJsonObject{{"found:x", 1}, {"edits", 2}, {"shown", 3}}},
            }},
            {"toured", QJsonObject{{"pinhole.series", true}, {"pinhole.screenshots", false}}},
        };
        PluginBench bench({{"pinhole.series", {}}, {"pinhole.screenshots", {}}, {"pinhole.info", {}}, {"pinhole.send", {}}});
        bench.plugins.reset();
        write(config, QJsonDocument(before).toJson());
        // A tour seen already in toured.json stays seen.
        QDir().mkpath(QFileInfo(Plugins::touredPath()).absolutePath());
        write(Plugins::touredPath(), R"({"pinhole.calendar": true})");
        auto plugins = std::make_unique<Plugins>(&bench.library, bench.service.get(), bench.viewer.get(), bench.translator.get(), &bench.history);
        // The settings as plugins read them: without it at once.
        QCOMPARE(plugins->settingsOf("pinhole.screenshots"), (QJsonObject{{"hide", true}}));
        QCOMPARE(plugins->settingsOf("pinhole.series"), (QJsonObject{{"recipe", QJsonObject{{"look", "warm"}}}}));
        QCOMPARE(plugins->settingsOf("pinhole.info"), QJsonObject());
        // And their state and cache with it.
        QCOMPARE(plugins->stateStore()->values("pinhole.series").value("edits:icloud:me@example.com").toObject(), edits);
        QCOMPARE(plugins->cacheStore()->values("pinhole.screenshots").value("found:folder:/x").toArray(), QJsonArray{"s3"});
        plugins->flush();
        const auto json = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        };
        // plugins.json: the switches and the settings, nothing else.
        const QJsonObject after{
            {"enabled", QJsonObject{{"pinhole.frame", false}, {"me.mine", true}}},
            {"settings", QJsonObject{
                {"pinhole.screenshots", QJsonObject{{"hide", true}}},
                {"pinhole.series", QJsonObject{{"recipe", QJsonObject{{"look", "warm"}}}}},
                {"pinhole.send", QJsonObject{{"compatible", true}}},
                {"pinhole.frame", QJsonObject{{"seconds", 5}}},
                {"me.mine", QJsonObject{{"found:x", 1}, {"edits", 2}, {"shown", 3}}},
            }},
        };
        QCOMPARE(json(config), after);
        QCOMPARE(json(Plugins::stateRoot() + "/pinhole.series/state.json"),
                 (QJsonObject{{"edits:icloud:me@example.com", edits},
                              {"edits", QJsonObject{{"manual", QJsonArray{}}, {"apart", QJsonArray{"d"}}}}}));
        QCOMPARE(json(Plugins::stateRoot() + "/pinhole.info/state.json"), (QJsonObject{{"shown", true}}));
        QCOMPARE(json(Plugins::stateRoot() + "/pinhole.send/state.json"),
                 (QJsonObject{{"devices", QJsonArray{QJsonObject{{"fingerprint", "f"}}}}}));
        QCOMPARE(json(Plugins::cacheRoot() + "/pinhole.screenshots/cache.json"),
                 (QJsonObject{{"found:icloud:me@example.com", QJsonArray{"s1", "s2"}}, {"found:folder:/x", QJsonArray{"s3"}}}));
        QCOMPARE(json(Plugins::touredPath()), (QJsonObject{{"pinhole.calendar", true}, {"pinhole.series", true}}));
        QVERIFY(!QFileInfo::exists(Plugins::stateRoot() + "/me.mine"));
        QVERIFY(!QFileInfo::exists(Plugins::cacheRoot() + "/me.mine"));
        QVERIFY(plugins->at("pinhole.series").value("toured").toBool());
        QVERIFY(!plugins->at("pinhole.screenshots").value("toured").toBool());

        // The next start writes nothing, not plugins.json, not the rest.
        const auto times = [&] {
            QStringList all;
            for (const QString &path : {config, Plugins::touredPath(), Plugins::stateRoot() + "/pinhole.series/state.json",
                                        Plugins::cacheRoot() + "/pinhole.screenshots/cache.json"})
                all << QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch()) + ' '
                           + QString::number(QFileInfo(path).size());
            return all;
        };
        const QStringList written = times();
        plugins.reset();
        QTest::qWait(20);
        plugins = std::make_unique<Plugins>(&bench.library, bench.service.get(), bench.viewer.get(), bench.translator.get(), &bench.history);
        QCOMPARE(plugins->stateStore()->values("pinhole.series").value("edits:icloud:me@example.com").toObject(), edits);
        plugins->flush();
        QCOMPARE(times(), written);

        // A tour seen goes to toured.json, not plugins.json.
        plugins->setToured("pinhole.screenshots");
        plugins->flush();
        QVERIFY(json(Plugins::touredPath()).value("pinhole.screenshots").toBool());
        QCOMPARE(times().first(), written.first());
        plugins.reset();
    }

    // Series' edits where an older Pinhole kept them, in plugins.json:
    // moved to its state as Pinhole starts, shown, and each edit after
    // kept there, undone and done again there; plugins.json, and its
    // `recipe`, a setting, as they were after the move.
    void seriesEditsMovedToItsStateStillApplyAndUndo() {
        PluginBench bench({{"pinhole.series", {}}});
        bench.plugins.reset();
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json";
        write(config, R"({"settings": {"pinhole.series": {"recipe": {"look": "warm"},
                          "edits:folder:/x": {"manual": [["a", "c"]], "apart": []}}}})");
        bench.plugins = std::make_unique<Plugins>(&bench.library, bench.service.get(), bench.viewer.get(), bench.translator.get(), &bench.history);
        bench.plugins->setEngine(&bench.engine);
        bench.plugins->libraryOpened();
        bench.plugins->flush();
        QCOMPARE(bench.plugins->settingsOf("pinhole.series"), (QJsonObject{{"recipe", QJsonObject{{"look", "warm"}}}}));
        const QDateTime moved = QFileInfo(config).lastModified();
        auto *host = qobject_cast<PluginHost *>(bench.plugins->at("pinhole.series").value("host").value<QObject *>());
        Keymap keymap;
        Theme theme(bench.dir.path());
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        bench.engine.rootContext()->setContextProperty("theme", &theme);
        Plugins::registerTypes();
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/series/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.series&load=0");
        std::unique_ptr<QObject> plugin(component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("series", bench.engine.newQObject(plugin.get()));
        // As if its finder found nothing: what was made by hand, shown.
        bench.js("series.take('')");
        QCOMPARE(bench.js("JSON.stringify(series.series)").toString(), QString("[[0,2]]"));
        const auto kept = [&] {
            bench.plugins->flush();
            QFile file(Plugins::stateRoot() + "/pinhole.series/state.json");
            return file.open(QIODevice::ReadOnly)
                       ? QJsonDocument::fromJson(file.readAll()).object().value("edits:folder:/x").toObject().value("manual").toArray()
                       : QJsonArray();
        };
        QCOMPARE(kept(), (QJsonArray{QJsonArray{"a", "c"}}));
        // b grouped with them: kept, then undone and done again.
        bench.js("series.group([1, 0])");
        QCOMPARE(bench.js("JSON.stringify(series.series)").toString(), QString("[[0,1,2]]"));
        QCOMPARE(kept(), (QJsonArray{QJsonArray{"b", "a", "c"}}));
        bench.history.undo();
        QCOMPARE(bench.js("JSON.stringify(series.series)").toString(), QString("[[0,2]]"));
        QCOMPARE(kept(), (QJsonArray{QJsonArray{"a", "c"}}));
        bench.history.redo();
        QCOMPARE(kept(), (QJsonArray{QJsonArray{"b", "a", "c"}}));
        QTest::qWait(20);
        bench.plugins->flush();
        QCOMPARE(QFileInfo(config).lastModified(), moved);
        QCOMPARE(bench.plugins->settingsOf("pinhole.series"), (QJsonObject{{"recipe", QJsonObject{{"look", "warm"}}}}));
        plugin.reset();
    }

    // Where they belong cannot be written: they stay in plugins.json, and
    // a setting changed meanwhile keeps them there too.
    void whatCannotMoveOutOfPluginsJsonStaysThere() {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json";
        PluginBench bench({{"pinhole.series", {}}});
        bench.plugins.reset();
        const QJsonObject before{{"settings", QJsonObject{{"pinhole.series", QJsonObject{{"edits:folder:/x", QJsonObject{{"manual", QJsonArray{}}, {"apart", QJsonArray{"a"}}}}}}}},
                                 {"toured", QJsonObject{{"pinhole.series", true}}}};
        write(config, QJsonDocument(before).toJson());
        // A file where its folder would be.
        QDir().mkpath(QFileInfo(Plugins::stateRoot()).absolutePath());
        write(Plugins::stateRoot(), "in the way");
        auto plugins = std::make_unique<Plugins>(&bench.library, bench.service.get(), bench.viewer.get(), bench.translator.get(), &bench.history);
        // A switch turned before it is known how the move went.
        plugins->setEnabled("pinhole.series", true);
        const auto json = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        };
        QJsonObject expected = before;
        expected.insert("enabled", QJsonObject{{"pinhole.series", true}});
        QCOMPARE(json(config), expected);
        QTRY_COMPARE(plugins->settingsOf("pinhole.series"), before["settings"]["pinhole.series"].toObject());
        // And so after it, too.
        plugins->setEnabled("pinhole.series", true);
        plugins->setEnabled("pinhole.series", false);
        expected.insert("enabled", QJsonObject{{"pinhole.series", false}});
        QCOMPARE(json(config), expected);
        plugins.reset();
        QVERIFY(QFile::remove(Plugins::stateRoot()));
        // The next start moves them.
        plugins = std::make_unique<Plugins>(&bench.library, bench.service.get(), bench.viewer.get(), bench.translator.get(), &bench.history);
        plugins->flush();
        expected.remove("settings");
        expected.remove("toured");
        expected.insert("settings", QJsonObject{});
        QCOMPARE(json(config), expected);
        QCOMPARE(plugins->stateStore()->values("pinhole.series").value("edits:folder:/x")["apart"].toArray(), QJsonArray{"a"});
        plugins.reset();
    }

    void aPluginAskingForANewerApiSaysSo() {
        PluginBench bench({{"t", {{"api", 99}}}, {"u", {{"api", Plugins::api}}}});
        QVERIFY(bench.plugins->at("t").value("error").toString().contains("99"));
        QVERIFY(!bench.plugins->at("t").value("enabled").toBool());
        QCOMPARE(bench.plugins->at("u").value("error").toString(), QString());
        QCOMPARE(bench.js("pinhole.api").toInt(), Plugins::api);
    }

    // `{ stream: true }` in fetch's third place: a URL to play, the
    // service's own while it still downloads, else the file's; without it
    // a path, as before. And each photo's Live effect as set on the iPhone.
    void aPluginFetchesAMotionToPlayAsItComesAndKnowsItsEffect() {
        PluginBench bench({{"t", {{"api", 3}}}}, {{"a", 1000}, {"b", 2000}, {"c", 3000}}, true);
        bench.js(R"(
            pinhole.fetch(0, "video", { stream: true }).then(url => said.push("a " + url))
            pinhole.fetch("b", "bounce", { stream: true }).then(url => said.push("b " + url))
            pinhole.fetch(2, "medium").then(path => said.push("c " + path))
        )");
        QList<QJsonObject> asked;
        QTRY_VERIFY((bench.fake.read(), asked = bench.fake.asked, asked.size() >= 3));
        auto params = [&](int k) { return asked[k].value("params").toObject(); };
        QCOMPARE(params(0).value("kind").toString(), QString("video"));
        QVERIFY(params(0).value("stream").toBool());
        QCOMPARE(params(1).value("kind").toString(), QString("bounce"));
        QVERIFY(params(1).value("stream").toBool());
        QVERIFY(!params(2).contains("stream"));
        bench.fake.send({{"id", asked[0].value("id")}, {"ok", true},
                         {"result", QJsonObject{{"path", "/cache/a.mov"}, {"stream", "http://127.0.0.1:1/a"}}}});
        QTRY_COMPARE(bench.said(), QStringList({"a http://127.0.0.1:1/a"}));
        bench.fake.send({{"id", asked[1].value("id")}, {"ok", true}, {"result", QJsonObject{{"path", "/cache/b bounce.mov"}}}});
        bench.fake.send({{"id", asked[2].value("id")}, {"ok", true}, {"result", QJsonObject{{"path", "/cache/c.jpg"}}}});
        QTRY_COMPARE(bench.said(), QStringList({"a http://127.0.0.1:1/a", "b file:///cache/b%20bounce.mov", "c /cache/c.jpg"}));

        // The effects: none in a library the service has given none to.
        QCOMPARE(bench.js("pinhole.library.item(0).effect").toString(), QString());
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "effects");
            db.setDatabaseName(bench.dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE assets ADD COLUMN live_effect TEXT"));
            QVERIFY(q.exec("UPDATE assets SET live_effect = 'bounce' WHERE id = 'b'"));
            QVERIFY(q.exec("UPDATE assets SET live_effect = 'long_exposure' WHERE id = 'c'"));
            db.close();
        }
        QSqlDatabase::removeDatabase("effects");
        QString error;
        QVERIFY(bench.library.open(bench.dir.path(), &error));
        QCOMPARE(bench.js("pinhole.library.item('b').effect").toString(), QString("bounce"));
        QCOMPARE(bench.js("pinhole.library.field('effect')").toVariant().toStringList(),
                 QStringList({"", "bounce", "long_exposure"}));
    }

    void aPluginNamesAPhotoByItsPositionOrItsId() {
        PluginBench bench({{"t", {}}});
        QCOMPARE(bench.js("pinhole.library.item('b').index").toInt(), 1);
        QCOMPARE(bench.js("pinhole.library.item(2).id").toString(), QString("c"));
        QVERIFY(bench.js("pinhole.library.item('zz').id").isUndefined());
        QVERIFY(bench.js("pinhole.library.item(3).id").isUndefined());
        bench.js(R"(pinhole.fetch("zz", "medium").then(null, e => said.push(e.code)))");
        QTRY_COMPARE(bench.said(), QStringList({"not_found"}));
    }

    void aCopiedImagePastesIntoAChatAsOne() {
        QTemporaryDir dir;
        // A JPEG bigger than 2560 px: as it is, and a PNG of 2560 at most.
        const QString jpeg = dir.path() + "/sheet.jpg";
        QImage big(3000, 1500, QImage::Format_RGB32);
        big.fill(Qt::red);
        QVERIFY(big.save(jpeg, "JPEG"));
        QFile file(jpeg);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        const Outgoing::ClipboardImage image = Outgoing::clipboardImage(jpeg);
        QCOMPARE(image.jpeg, bytes);
        QImage png;
        QVERIFY(png.loadFromData(image.png, "PNG"));
        QCOMPARE(png.size(), QSize(2560, 1280));
        // A GIF goes as itself, to move, and its first frame as a PNG for
        // what takes only that; what is not an image, as a file only.
        const QString gif = dir.path() + "/clip.gif";
        write(gif, QByteArray("GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00\x00\x00\x00,"
                              "\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;", 43));
        const Outgoing::ClipboardImage moving = Outgoing::clipboardImage(gif);
        QCOMPARE(moving.gif.size(), 43);
        QImage frame;
        QVERIFY(frame.loadFromData(moving.png, "PNG"));
        QCOMPARE(frame.size(), QSize(1, 1));
        QVERIFY(moving.jpeg.isEmpty());
        write(dir.path() + "/notes.txt", "not a picture");
        const Outgoing::ClipboardImage none = Outgoing::clipboardImage(dir.path() + "/notes.txt");
        QVERIFY(none.jpeg.isEmpty() && none.png.isEmpty() && none.gif.isEmpty());

        // A plugin's copyFile, as Pinhole's own copy: the file, and the
        // image; its promise answered once the clipboard has them, not
        // before, when a paste would still be what was there.
        PluginBench bench({{"t", {}}});
        QVERIFY(bench.host);
        QGuiApplication::clipboard()->clear();
        auto mimeData = [] { return QGuiApplication::clipboard()->mimeData(); };
        QStringList saidAsItWasSet = {"not set"};
        QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, &bench.engine, [&] {
            if (mimeData() && mimeData()->hasFormat("image/png"))
                saidAsItWasSet = bench.said();
        });
        bench.engine.globalObject().setProperty("jpeg", jpeg);
        bench.engine.globalObject().setProperty("gif", gif);
        bench.js("pinhole.copyFile(jpeg).then(path => said.push('copied ' + path))");
        QCOMPARE(bench.said(), QStringList());
        QTRY_COMPARE(bench.said(), QStringList({"copied " + jpeg}));
        QCOMPARE(saidAsItWasSet, QStringList());
        const QMimeData *mime = mimeData();
        QVERIFY(mime && mime->hasFormat("image/png"));
        QCOMPARE(mime->data("image/jpeg"), bytes);
        QCOMPARE(mime->urls(), QList<QUrl>({QUrl::fromLocalFile(jpeg)}));
        QVERIFY(mime->data("x-special/gnome-copied-files").startsWith("copy\nfile://"));
        // A GIF: itself and its first frame.
        bench.js("pinhole.copyFile(gif).then(() => said.push('gif'))");
        QTRY_COMPARE(bench.said().size(), 2);
        QVERIFY(mimeData()->hasFormat("image/gif") && mimeData()->hasFormat("image/png"));
        // No such file: said so, the clipboard as it was.
        bench.js(R"(pinhole.copyFile("/no/such/file.jpg").then(null, e => said.push(e.code)))");
        QTRY_COMPARE(bench.said().size(), 3);
        QCOMPARE(bench.said().last(), QString("not_found"));
        QVERIFY(mimeData()->hasFormat("image/gif"));
    }

    void aSelfTestCopiesToItsOwnClipboardAndNeverToRealPictures() {
        // The account's own Pictures, whatever HOME says; a scratch one is
        // not.
        const QString home = QString::fromLocal8Bit(getpwuid(getuid())->pw_dir);
        QVERIFY(isAccountPictures(home + "/Pictures"));
        QVERIFY(isAccountPictures(home + "/Pictures/Pinhole/sheet.jpg"));
        QVERIFY(!isAccountPictures(home + "/PicturesOther"));
        QTemporaryDir scratch;
        QVERIFY(!isAccountPictures(scratch.path() + "/Pictures"));
        // In a self-test a copy is kept in the window, the session's
        // clipboard as it was; Pictures is the one given.
        QGuiApplication::clipboard()->clear();
        qputenv("PINHOLE_SELFTEST", "test");
        qputenv("XDG_PICTURES_DIR", (scratch.path() + "/Pictures").toUtf8());
        auto *mime = new QMimeData;
        mime->setText("kept");
        setClipboard(mime);
        QCOMPARE(clipboardNow()->text(), QString("kept"));
        const QMimeData *session = QGuiApplication::clipboard()->mimeData();
        QVERIFY(!session || !session->hasText());
        QCOMPARE(picturesLocation(), scratch.path() + "/Pictures");
        qunsetenv("PINHOLE_SELFTEST");
        qunsetenv("XDG_PICTURES_DIR");
        QCOMPARE(picturesLocation(), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    }

    void theLastCopyAskedForWinsTheClipboard() {
        // A big photo's PNG takes a while; a small one asked for just
        // after is ready first. The clipboard ends with the small one, and
        // the big one, never set, is said cancelled, not copied.
        QTemporaryDir dir;
        const QString big = dir.path() + "/big.jpg", small = dir.path() + "/small.jpg";
        QImage noise(6000, 4000, QImage::Format_RGB32);
        quint32 seed = 1;
        for (int y = 0; y < noise.height(); ++y) {
            auto *line = reinterpret_cast<quint32 *>(noise.scanLine(y));
            for (int x = 0; x < noise.width(); ++x)
                line[x] = (seed = seed * 1664525u + 1013904223u) | 0xff000000u;
        }
        QVERIFY(noise.save(big, "JPEG", 95));
        QImage dot(4, 4, QImage::Format_RGB32);
        dot.fill(Qt::blue);
        QVERIFY(dot.save(small, "JPEG"));

        PluginBench bench({{"t", {}}});
        QVERIFY(bench.host);
        QGuiApplication::clipboard()->clear();
        QStringList set;
        QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, &bench.engine, [&] {
            if (const QMimeData *mime = QGuiApplication::clipboard()->mimeData(); mime && !mime->urls().isEmpty())
                set << QFileInfo(mime->urls().first().toLocalFile()).fileName();
        });
        bench.engine.globalObject().setProperty("big", big);
        bench.engine.globalObject().setProperty("small", small);
        bench.js(R"(pinhole.copyFile(big).then(() => said.push("big copied"), e => said.push("big " + e.code));
                   pinhole.copyFile(small).then(() => said.push("small copied"), e => said.push("small " + e.code)))");
        QTRY_COMPARE_WITH_TIMEOUT(bench.said().size(), 2, 30000);
        QStringList said = bench.said();
        said.sort();
        QCOMPARE(said, QStringList({"big cancelled", "small copied"}));
        QCOMPARE(set, QStringList({"small.jpg"}));
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls(), QList<QUrl>({QUrl::fromLocalFile(small)}));

        // Pinhole's own, one after another: each set as it is ready.
        set.clear();
        Outgoing::toClipboard({small}, &bench.engine);
        QTRY_COMPARE(set, QStringList({"small.jpg"}));
        bool dropped = false, then = false;
        Outgoing::toClipboard({big}, &bench.engine, [&] { then = true; }, [&] { dropped = true; });
        Outgoing::toClipboard({small, big}, &bench.engine);
        QTRY_COMPARE_WITH_TIMEOUT(set.size(), 2, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(dropped, 30000);
        QVERIFY(!then);
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls(),
                 QList<QUrl>({QUrl::fromLocalFile(small), QUrl::fromLocalFile(big)}));
    }

    void aPluginsNoticeIsSaidAsItsOwn() {
        PluginBench bench({{"t", {}}});
        QSignalSpy notice(bench.plugins.get(), &Plugins::notice);
        bench.js(R"(pinhole.notify("Saved to Pictures"))");
        QCOMPARE(notice.size(), 1);
        QCOMPARE(notice.first(), QVariantList({"t", "Saved to Pictures"}));
    }

    // A photo shown is fetched at once, and its neighbours beside it; those
    // passed under a held key are not, only the one it stops on.
    void aPhotoShownIsFetchedAtOnceAndThosePassedQuicklyAreNot() {
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1000}, {"b", 2000}, {"c", 3000}}, true);
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(dir.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();

        Viewer viewer(&library, &service);
        viewer.openAt(0, {});
        // A key changes photo at once, with nothing sliding, so two
        // neighbours can be compared; only the carousel glides after it.
        viewer.setViewport(800, 600);
        auto mediums = [&fake] {
            fake.read();
            return std::count_if(fake.asked.cbegin(), fake.asked.cend(), [](const QJsonObject &line) {
                return line.value("params").toObject().value("kind").toString() == QLatin1String("medium");
            });
        };
        QTRY_COMPARE(mediums(), 3); // a, and b and c beside it
        QTest::qWait(200);
        viewer.step(1);
        QCOMPARE(viewer.index(), 1);
        QCOMPARE(viewer.offset(), 0.0);
        QVERIFY(viewer.stripOffset() > 0);
        QTRY_COMPARE(mediums(), 6);
        // Photos passing under a held key are not fetched; the one it
        // stops on is, a moment later.
        QTest::qWait(200);
        viewer.step(1); // the first press: fetched at once
        viewer.step(-1);
        viewer.step(-1);
        QCOMPARE(viewer.index(), 0);
        QTest::qWait(50);
        QCOMPARE(mediums(), 9);
        QTRY_COMPARE(mediums(), 12);
        QTest::qWait(100);
        QCOMPARE(mediums(), 12);
        // Nothing of a Live Photo's motion: that is the Live Photos plugin's.
        QCOMPARE(fake.live().size(), 0);
        qunsetenv("PINHOLE_SOCKET");
    }

    // plugins/live as the window loads it, over a library of Live Photos
    // and a stand-in service: L asks for the motion once however often it
    // is pressed, and plays it as it comes (the service's stream); a
    // motion that does not come is said, in so many words; a bounce chosen
    // is asked for and goes round; another photo's layer starts afresh.
    void theLivePhotosPluginPlaysTheMotionAsItComesAndSaysWhyNot() {
        PluginBench bench({{"pinhole.live", {{"api", 3}}}}, {{"a", 1000}, {"b", 2000}, {"c", 3000}}, true);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "live");
            db.setDatabaseName(bench.dir.path() + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE assets SET is_live = 1"));
            QVERIFY(q.exec("ALTER TABLE assets ADD COLUMN live_effect TEXT"));
            QVERIFY(q.exec("UPDATE assets SET live_effect = 'loop' WHERE id = 'c'"));
            db.close();
        }
        QSqlDatabase::removeDatabase("live");
        QString error;
        QVERIFY(bench.library.open(bench.dir.path(), &error));
        Keymap keymap;
        bench.engine.rootContext()->setContextProperty("keymap", &keymap);
        Plugins::registerTypes();
        const QString real = QStringLiteral(PINHOLE_I18N "/../../../plugins/live/Main.qml");
        QQmlComponent component(&bench.engine, QUrl::fromLocalFile(real).toString() + "?plugin=pinhole.live&load=0");
        std::unique_ptr<QObject> plugin(component.createWithInitialProperties({{"pinhole", QVariant::fromValue<QObject *>(bench.host)}}));
        QVERIFY2(plugin, qPrintable(component.errorString()));
        QQmlEngine::setObjectOwnership(plugin.get(), QQmlEngine::CppOwnership);
        bench.engine.globalObject().setProperty("plugin", bench.engine.newQObject(plugin.get()));
        bench.js("plugin.playAction.complained.connect(text => said.push('said ' + text))");
        // What it said, but that the stand-in's URLs do not play.
        auto told = [&bench] {
            QStringList said = bench.said();
            said.removeAll("said Could not play the motion");
            return said;
        };
        auto fetches = [&bench](const QString &kind) {
            bench.fake.read();
            QList<QJsonObject> found;
            for (const QJsonObject &line : std::as_const(bench.fake.asked))
                if (line.value("params").toObject().value("kind").toString() == kind)
                    found.append(line);
            return found;
        };

        // Its mark: the library's field, off until turned on.
        QCOMPARE(bench.js("plugin.marks[0].field").toString(), QString("live"));
        QVERIFY(!bench.js("plugin.marks[0].defaultShown").toBool());
        // Nothing to show with no photo open.
        QVERIFY(!bench.js("plugin.playAction.shown").toBool());
        bench.viewer->openAt(0, {});
        QVERIFY(bench.js("plugin.playAction.shown").toBool());
        QCOMPARE(bench.js("plugin.playAction.text").toString(), QString("Live"));
        bench.js("layer = plugin.viewerLayer.createObject(null, { photo: 0, shown: true })");
        QVERIFY(bench.js("plugin.liveLayer === layer").toBool());
        // On its way at once, and asked for once however often L is pressed.
        bench.js("plugin.playAction.triggered(); plugin.playAction.triggered(); plugin.playAction.triggered()");
        QVERIFY(bench.js("plugin.playAction.active && plugin.playAction.busy").toBool());
        QTRY_COMPARE(fetches("live").size(), 1);
        QTest::qWait(50);
        QCOMPARE(fetches("live").size(), 1);
        QVERIFY(fetches("live")[0].value("params").toObject().value("stream").toBool());
        bench.fake.send({{"id", fetches("live")[0].value("id")}, {"ok", true},
                         {"result", QJsonObject{{"path", "/cache/a.mov"}, {"stream", "http://127.0.0.1:1/a"}}}});
        QTRY_VERIFY(!bench.js("layer.asking").toBool());
        QCOMPARE(bench.js("layer.source").toString(), QString("http://127.0.0.1:1/a"));
        QCOMPARE(bench.js("layer.sourceKind").toString(), QString("live"));
        // Here: played again without asking.
        bench.js("plugin.playAction.triggered()");
        QTest::qWait(50);
        QCOMPARE(fetches("live").size(), 1);
        // Gone as the photo is: stopped, and no longer the plugin's.
        bench.js("layer.shown = false");
        QVERIFY(!bench.js("layer.playing").toBool());
        QVERIFY(bench.js("plugin.liveLayer === null").toBool());
        bench.js("layer.destroy()");

        // Not there: said under the button, in so many words.
        bench.viewer->step(1);
        bench.js("layer = plugin.viewerLayer.createObject(null, { photo: 1, shown: true }); said.length = 0");
        bench.js("plugin.playAction.triggered()");
        QTRY_COMPARE(fetches("live").size(), 2);
        bench.fake.send({{"id", fetches("live")[1].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "not_found"}, {"message", "no motion"}}}});
        QTRY_COMPARE(told(), QStringList({"said This Live Photo has no motion"}));
        QVERIFY(!bench.js("plugin.playAction.active").toBool());
        bench.js("plugin.playAction.triggered()");
        QTRY_COMPARE(fetches("live").size(), 3);
        bench.fake.send({{"id", fetches("live")[2].value("id")}, {"ok", false},
                         {"error", QJsonObject{{"code", "needs_sign_in"}, {"message", "no session"}}}});
        QTRY_COMPARE(told().size(), 2);
        QCOMPARE(told()[1], QString("said Not signed in to x"));

        // A bounce chosen: asked for to play, round and round, at once.
        bench.js("plugin.choose('bounce')");
        QCOMPARE(bench.js("plugin.playAction.text").toString(), QString("Bounce"));
        QVERIFY(bench.js("layer.loading").toBool());
        QTRY_COMPARE(fetches("bounce").size(), 1);
        bench.fake.send({{"id", fetches("bounce")[0].value("id")}, {"ok", true}, {"result", QJsonObject{{"path", "/cache/b bounce.mov"}}}});
        QTRY_COMPARE(bench.js("layer.source").toString(), QString("file:///cache/b%20bounce.mov"));
        QVERIFY(!bench.js("layer.loading").toBool());
        QVERIFY(bench.js("layer.looping").toBool());
        // Only while Pinhole runs, and for that photo: back to Live, it is
        // forgotten; the next photo has its own.
        QCOMPARE(bench.js("plugin.chosen['b']").toString(), QString("bounce"));
        bench.js("layer.shown = false; layer.destroy()");

        // A loop set on the iPhone: "iPhone" in its menu, played as it shows.
        bench.viewer->step(1);
        QCOMPARE(bench.js("plugin.saved").toString(), QString("loop"));
        QCOMPARE(bench.js("plugin.actions[2].hint").toString(), QString("iPhone"));
        QCOMPARE(bench.js("plugin.actions[3].hint").toString(), QString());
        bench.js("layer = plugin.viewerLayer.createObject(null, { photo: 2, shown: true })");
        QTRY_COMPARE(fetches("loop").size(), 1);
        bench.fake.send({{"id", fetches("loop")[0].value("id")}, {"ok", false}, {"error", QJsonObject{{"code", "unavailable"}}}});
        QTRY_COMPARE(told().size(), 3);
        QCOMPARE(told()[2], QString("said Could not show this effect"));
        QVERIFY(!bench.js("layer.loading").toBool());
        bench.js("layer.shown = false; layer.destroy()");
    }

    void aDragOutGivesItsFilesToTheDropAndNothingAsItStarts() {
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir dir;
        makeLibrary(dir.path(), {{"a", 1000}});
        Library library;
        QString error;
        QVERIFY(library.open(dir.path(), &error));
        QFile photo(dir.path() + "/a.jpg");
        QVERIFY(photo.open(QIODevice::WriteOnly));
        photo.write("jpeg");
        photo.close();
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(dir.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();
        // Answered as pinholed does, while the drop waits for it.
        QObject::connect(fake.peer, &QLocalSocket::readyRead, fake.peer, [&fake, &photo] {
            const qsizetype seen = fake.asked.size();
            fake.read();
            for (qsizetype i = seen; i < fake.asked.size(); ++i)
                if (fake.asked[i].value("method") == QLatin1String("fetch"))
                    fake.send({{"id", fake.asked[i].value("id")}, {"ok", true},
                               {"result", QJsonObject{{"path", photo.fileName()}, {"type", "public.jpeg"}}}});
        });

        Outgoing outgoing(&library, &service);
        std::unique_ptr<QMimeData> data(outgoing.dragData(outgoing.snapshot({0})));
        // As Qt's Wayland drag starts, it asks for the URL list (to offer a
        // portal transfer): nothing, at once, and nothing fetched.
        QVERIFY(data->hasUrls());
        QVERIFY(data->urls().isEmpty());
        QTest::qWait(50);
        fake.read();
        QVERIFY(fake.asked.isEmpty());
        // A drop target's request, which Qt 6.12 also makes through urls():
        // the photo, under its camera name (a JPEG, so named one).
        const QList<QUrl> urls = data->urls();
        QCOMPARE(urls.size(), 1);
        QCOMPARE(QFileInfo(urls.first().toLocalFile()).fileName(), QString("a.jpg"));
        QVERIFY(QFileInfo::exists(urls.first().toLocalFile()));
        QCOMPARE(data->data("text/uri-list"), urls.first().toEncoded() + "\r\n");
        qunsetenv("PINHOLE_SOCKET");
    }

    // Storage::library names a folder's library as the service does
    // (storage::library_for); one made under its former name is kept to.
    void aFoldersLibraryIsNamedAsTheServiceNamesIt() {
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir location;
        const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/settings.json";
        QDir().mkpath(QFileInfo(settingsPath).path());
        const auto shown = [&](const QString &path) {
            QSaveFile file(settingsPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(QJsonObject{{"location", location.path()},
                                                 {"source", QJsonObject{{"kind", "folder"}, {"path", path}}}})
                           .toJson());
            QVERIFY(file.commit());
        };
        shown("/home/me/Pictures");
        QCOMPARE(Storage::library(), location.path() + "/library/folder/home-me-Pictures");
        // The same hash as core's folder test of this path.
        shown("/home/me/My-Photos");
        QCOMPARE(Storage::library(), location.path() + "/library/folder/home-me-My-Photos~7f094097f3a39b1b");
        // A tilde of its own is told apart from the hash's.
        shown("/home/me/My/Photos~7f094097f3a39b1b");
        QVERIFY(Storage::library() != location.path() + "/library/folder/home-me-My-Photos~7f094097f3a39b1b");
        // The same as core's: one made under a former name is kept to, and
        // a folder whose name another's former one is has one by its id.
        const QString former = location.path() + "/library/folder/a-b-c";
        QVERIFY(QDir().mkpath(former));
        makeLibrary(former, {{"x.jpg", 1}}, false, "folder:/a/b-c");
        shown("/a/b-c");
        QCOMPARE(Storage::library(), former);
        shown("/a/b/c");
        QCOMPARE(Storage::library(), former + "~43215282df502c0e");
        shown("/a-b/c");
        QVERIFY(Storage::library() != former);
        // That name by its id another's too: the next; its own there is
        // kept to, even once its name is no one's.
        const QString byId = former + "~43215282df502c0e", next = byId + "-1";
        QVERIFY(QDir().mkpath(byId) && QDir().mkpath(next));
        makeLibrary(byId, {{"x.jpg", 1}}, false, "folder:/a/b/c~43215282df502c0e");
        shown("/a/b/c");
        QCOMPARE(Storage::library(), next);
        makeLibrary(next, {{"x.jpg", 1}}, false, "folder:/a/b/c");
        QVERIFY(QDir(former).removeRecursively());
        QCOMPARE(Storage::library(), next);
        QVERIFY(QDir(byId).removeRecursively());
        QCOMPARE(Storage::library(), next);
        QFile::remove(settingsPath);
    }

    // An encoder slow to read (ffmpeg making a GIF) is given frames as it
    // takes them, and the window's thread never waits on it meanwhile.
    void framesGoToASlowEncoderWithoutTheWindowWaiting() {
        QTemporaryDir dir;
        const QSize size(400, 300);
        const int frames = 30;
        FrameEncoder encoder("sh", {"-c", "sleep 1; cat > \"$0\"", dir.filePath("out")}, size);
        QImage frame(size, QImage::Format_RGB32);
        frame.fill(Qt::red);
        int added = 0, heldBack = 0;
        QString error = "not finished";
        // A frame a turn, as a recording's come (one grab after another).
        std::function<void()> more = [&] {
            if (added == frames)
                return encoder.end();
            ++added;
            if (encoder.add(frame))
                QTimer::singleShot(0, this, more);
            else
                ++heldBack;
        };
        connect(&encoder, &FrameEncoder::ready, this, more);
        connect(&encoder, &FrameEncoder::finished, this, [&](const QString &e) { error = e; });
        QElapsedTimer clock;
        qint64 last = 0, longest = 0;
        QTimer tick;
        connect(&tick, &QTimer::timeout, this, [&] {
            longest = std::max(longest, clock.elapsed() - last);
            last = clock.elapsed();
        });
        clock.start();
        tick.start(10);
        encoder.start();
        // Once it is running (and asleep).
        QTimer::singleShot(200, this, more);
        QTRY_VERIFY_WITH_TIMEOUT(error != "not finished", 20000);
        QCOMPARE(error, QString());
        QCOMPARE(added, frames);
        QVERIFY2(longest < 300, qPrintable(QString("the window waited %1 ms").arg(longest)));
        QVERIFY(heldBack > 0);
        QCOMPARE(QFileInfo(dir.filePath("out")).size(), qint64(frames) * size.width() * size.height() * 4);

        // One that fails says how, and one that is not there says so.
        FrameEncoder failing("sh", {"-c", "echo broken >&2; exit 3"}, size);
        QString failed;
        connect(&failing, &FrameEncoder::finished, this, [&](const QString &e) { failed = e; });
        failing.start();
        QTRY_COMPARE(failed, QString("broken"));
        FrameEncoder missing("/nonexistent/ffmpeg", {}, size);
        QString absent;
        connect(&missing, &FrameEncoder::finished, this, [&](const QString &e) { absent = e; });
        missing.start();
        QTRY_VERIFY(absent.contains("did not start"));
    }
};

QTEST_MAIN(PinholeTests)
#include "pinhole_tests.moc"
