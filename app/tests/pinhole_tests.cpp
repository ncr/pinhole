#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "library.h"
#include "outgoing.h"
#include "scrollphysics.h"
#include "theme.h"
#include "translator.h"
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

    // A library in `dir` holding `rows` (id, date) and nothing else of note.
    static void makeLibrary(const QString &dir, const QList<std::pair<QByteArray, int>> &rows) {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "make");
            db.setDatabaseName(dir + "/library.sqlite");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE assets (id TEXT, thumb_path TEXT, avg_color INTEGER, thumb_w INTEGER, thumb_h INTEGER,"
                           " duration REAL, asset_date INTEGER, is_video INTEGER, is_live INTEGER, favorite INTEGER, filename TEXT)"));
            for (const auto &[id, date] : rows) {
                q.prepare("INSERT INTO assets VALUES (?, '', 0, 1, 1, 0, ?, 0, 0, 0, ?)");
                q.addBindValue(QString::fromUtf8(id));
                q.addBindValue(date);
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
    }

    void englishIsTheFallback() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", PINHOLE_I18N);
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
        QCOMPARE(translator.translate("Outgoing", "Copied %n photo(s)", nullptr, 1), QString("Copied %n photo"));
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
};

QTEST_MAIN(PinholeTests)
#include "pinhole_tests.moc"
