#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "library.h"
#include "outgoing.h"
#include "photogrid.h"
#include "thumbnailloader.h"
#include "scrollphysics.h"
#include "serviceclient.h"
#include "theme.h"
#include "translator.h"
#include "viewer.h"
#include "viewermotion.h"

class PinholeTests : public QObject {
    Q_OBJECT

private:
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
    static void makeLibrary(const QString &dir, const QList<std::pair<QByteArray, int>> &rows, bool live = false) {
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
            db.close();
        }
        QSqlDatabase::removeDatabase("make");
    }

    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

private slots:
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
        QCOMPARE(grid.rectOf(4).left(), 200.0);
        QCOMPARE(grid.indexAt(150, 50), 1);
        QCOMPARE(grid.indexAt(250, 50), 4);

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

    void whatGoesOutIsWhatWasChosenThroughAReload() {
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir before, after;
        makeLibrary(before.path(), {{"a", 1000}, {"b", 2000}});
        makeLibrary(after.path(), {{"new", 500}, {"a", 1000}, {"b", 2000}});
        Library library, reloaded;
        QString error;
        QVERIFY(library.open(before.path(), &error));
        QVERIFY(reloaded.open(after.path(), &error));

        Outgoing outgoing(&library, nullptr);
        const QList<LibraryItem> chosen = outgoing.snapshot({1, 7});
        QCOMPARE(chosen.size(), 1); // a position past the end is left out
        library.swap(reloaded);
        QCOMPARE(library.at(1).id, QByteArray("a"));
        QCOMPARE(chosen.first().id, QByteArray("b"));
        QCOMPARE(chosen.first().filename, QString("b.HEIC"));
    }

private:
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
    };

private slots:
    void aLivePhotosMotionIsAskedForOnceAndSaysWhyItDidNotCome() {
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
        fake.send({{"event", "source"}, {"kind", "icloud"}, {"name", "iCloud"}, {"needs_sign_in", true}});
        QTRY_VERIFY(service.sourceName() == QLatin1String("iCloud"));

        Viewer viewer(&library, &service);
        QSignalSpy failed(&viewer, &Viewer::liveFailed);
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
        QVERIFY(!viewer.liveLoading());
        // On its way at once, and asked for once however often it is pressed.
        viewer.playLive();
        QVERIFY(viewer.liveLoading());
        viewer.playLive();
        viewer.playLive();
        QTRY_COMPARE(fake.live().size(), 1);
        QTest::qWait(50);
        QCOMPARE(fake.live().size(), 1);
        fake.send({{"id", fake.live()[0].value("id")}, {"ok", true},
                   {"result", QJsonObject{{"path", "/cache/a.mov"}, {"stream", "http://127.0.0.1:1/a"}}}});
        QTRY_VERIFY(!viewer.liveLoading());
        QCOMPARE(viewer.liveSource(), QUrl("http://127.0.0.1:1/a"));
        // Here: played again without asking.
        viewer.playLive();
        QVERIFY(!viewer.liveLoading());
        QCOMPARE(fake.live().size(), 1);

        // Not signed in: said, in so many words.
        viewer.step(1);
        viewer.playLive();
        QTRY_COMPARE(fake.live().size(), 2);
        fake.send({{"id", fake.live()[1].value("id")}, {"ok", false},
                   {"error", QJsonObject{{"code", "needs_sign_in"}, {"message", "no session"}}}});
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!viewer.liveLoading());
        QCOMPARE(failed[0][0].toString(), QString("Not signed in to iCloud"));

        // Offline: the service says so before it answers.
        viewer.playLive();
        QTRY_COMPARE(fake.live().size(), 3);
        fake.send({{"event", "health"}, {"state", "offline"}});
        fake.send({{"id", fake.live()[2].value("id")}, {"ok", false},
                   {"error", QJsonObject{{"code", "unavailable"}, {"message", "network"}}}});
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(failed[1][0].toString(), QString("Offline, and the motion is not on this computer"));

        // Moving on drops the wait, and a late answer for the photo left.
        viewer.playLive();
        QTRY_COMPARE(fake.live().size(), 4);
        viewer.step(1);
        QVERIFY(!viewer.liveLoading());
        fake.send({{"id", fake.live()[3].value("id")}, {"ok", false}, {"error", QJsonObject{{"code", "not_found"}}}});
        QTest::qWait(50);
        QCOMPARE(failed.size(), 2);
        QVERIFY(viewer.liveSource().isEmpty());
        qunsetenv("PINHOLE_SOCKET");
    }
};

QTEST_MAIN(PinholeTests)
#include "pinhole_tests.moc"
