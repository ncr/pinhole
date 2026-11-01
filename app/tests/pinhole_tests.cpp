#include <QClipboard>
#include <QDirIterator>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <csignal>

#include "encoder.h"
#include "favourites.h"
#include "history.h"
#include "keymap.h"
#include "library.h"
#include "libraryreloader.h"
#include "outgoing.h"
#include "photogrid.h"
#include "plugins.h"
#include "photoprovider.h"
#include "thumbnailloader.h"
#include "scrollphysics.h"
#include "serviceclient.h"
#include "storage.h"
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

    // Plugins as the window has them, each of `manifests` (its id, and
    // what its manifest.json says besides) a folder with a Main.qml, and
    // `pinhole` in the engine's JavaScript the first one's host. Settings
    // go to the test's own config folder, started empty.
    struct PluginBench {
        QTemporaryDir dir;
        Library library;
        std::unique_ptr<ServiceClient> service;
        std::unique_ptr<Viewer> viewer;
        std::unique_ptr<Translator> translator;
        History history;
        QQmlEngine engine;
        std::unique_ptr<Plugins> plugins;
        PluginHost *host = nullptr;

        explicit PluginBench(const QList<std::pair<QString, QJsonObject>> &manifests,
                             const QList<std::pair<QByteArray, qint64>> &photos = {{"a", 1000}, {"b", 2000}, {"c", 3000}}) {
            QStandardPaths::setTestModeEnabled(true);
            QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json");
            makeLibrary(dir.path(), photos, false, "folder:/x");
            QString error;
            library.open(dir.path(), &error);
            qputenv("PINHOLE_SOCKET", (dir.path() + "/none.sock").toUtf8());
            service = std::make_unique<ServiceClient>(dir.path());
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
        QCOMPARE(Plugins::apiTwoUses(dir.path()), QStringList({"hidden", "avgColor"}));
        write(dir.path() + "/Main.qml", "import QtQuick\nQtObject {}\n");
        write(dir.path() + "/Helper.js", "function go() { pinhole.run('x', []).then(r => r) }\n");
        QCOMPARE(Plugins::apiTwoUses(dir.path()), QStringList({"promises"}));
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
        QCOMPARE(Plugins::apiTwoUses(dir.path()), QStringList());
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
        QCOMPARE(Plugins::apiTwoUses(dir.path()), QStringList({"pinhole.coveredBy", "hidden", "avgColor", "run's output"}));
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
            QCOMPARE(Plugins::apiTwoUses(dir.path()), QStringList({"avgColor"}));
        }
        // Pinhole's own: each that uses any asks for API 2, and what some
        // use is found.
        const QString builtIn = QStringLiteral(PINHOLE_I18N "/../../../plugins");
        for (const QFileInfo &plugin : QDir(builtIn).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile manifest(plugin.filePath() + "/manifest.json");
            QVERIFY(manifest.open(QIODevice::ReadOnly));
            const int api = QJsonDocument::fromJson(manifest.readAll()).object().value("api").toInt(1);
            const QStringList uses = Plugins::apiTwoUses(plugin.filePath());
            QVERIFY2(api >= 2 || uses.isEmpty(), qPrintable(plugin.fileName() + " uses " + uses.join(", ") + " and asks for API 1"));
        }
        QVERIFY(Plugins::apiTwoUses(builtIn + "/screenshots").contains("hidden"));
        QVERIFY(Plugins::apiTwoUses(builtIn + "/send").contains("run's output"));
        QVERIFY(Plugins::apiTwoUses(builtIn + "/colour").contains("pinhole.coveredBy"));
        QVERIFY(Plugins::apiTwoUses(builtIn + "/colour").contains("pinhole.dismissed"));
        QVERIFY(Plugins::apiTwoUses(builtIn + "/colour").contains("avgColor"));
        QVERIFY(Plugins::apiTwoUses(builtIn + "/frame").contains("pinhole.keepAwake"));
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
        QCOMPARE(grid.indexAt(50, 50), 1);
        QCOMPARE(grid.indexAt(150, 50), 3);
        // A hidden photo's cell is where it would be: the next shown.
        QCOMPARE(grid.rectOf(0), grid.rectOf(1));
        QCOMPARE(grid.rectOf(2), grid.rectOf(3));
        QVERIFY(grid.isHidden(2) && !grid.isHidden(3));
        QCOMPARE(grid.hiddenCount(), 2);
        QCOMPARE(grid.rectOf(4).left(), 200.0);
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
        QCOMPARE(grid.indexAt(150, 50), 2);
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
        QCOMPARE(grid.indexAt(50, 50), 0);
        QCOMPARE(grid.indexAt(150, 50), 2);
        QCOMPARE(grid.indexAt(250, 50), 4);

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
        QCOMPARE(grid.indexAt(350, 650), 199); // 200 photos, a row of 4 last
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
        grid.press(QPointF(350, 50));
        grid.setHidden({0});
        grid.release(QPointF(350, 50));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().first().toInt(), 3);
        // Pressed on d, released after d was hidden: nothing opens.
        grid.setHidden({});
        grid.press(QPointF(350, 50));
        grid.setHidden({3});
        grid.release(QPointF(350, 50));
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
        bench.plugins->setSettingOf("pinhole.screenshots", "found:folder:/x", QJsonArray{"b"});
        // And of a source taken out since.
        bench.plugins->setSettingOf("pinhole.screenshots", "found:folder:/gone", QJsonArray{"z"});
        // The sources added: both libraries'.
        const QString settings = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/settings.json";
        QDir().mkpath(QFileInfo(settings).absolutePath());
        write(settings, R"({"sources": [{"kind": "folder", "path": "/x"}, {"kind": "folder", "path": "/y"}]})");
        QCOMPARE(Storage::sourceIds(), (QStringList{"folder:/x", "folder:/y"}));
        // From before, kept in its cacheDirs: gone as it loads.
        const QString oldCache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/plugins/pinhole.screenshots";
        QDir().mkpath(oldCache + "/folder-x");
        write(oldCache + "/folder-x/screenshots", "b\n");
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
        QJsonObject kept = bench.plugins->settingsOf("pinhole.screenshots");
        QCOMPARE(kept.value("found:folder:/y").toArray(), QJsonArray{"y"});
        QCOMPARE(kept.value("found:folder:/x").toArray(), QJsonArray{"b"});
        // What it found of a source no longer added is gone, and so is
        // what it kept before in its cacheDirs.
        QVERIFY(!kept.contains("found:folder:/gone"));
        QTRY_VERIFY(!QFileInfo::exists(oldCache));
        QVERIFY(bench.js("pinhole.settings['found:folder:/gone']").isUndefined());

        // Too many to keep: none kept for that library, rather than a list
        // every plugin's settings would be slow with.
        QVERIFY(bench.js("shots.keep('found:folder:/y', Array.from({ length: 5001 }, (_, i) => 'p' + i))").toBool());
        kept = bench.plugins->settingsOf("pinhole.screenshots");
        QVERIFY(!kept.contains("found:folder:/y"));
        QVERIFY(bench.js("shots.keep('found:folder:/y', Array.from({ length: 5000 }, (_, i) => 'p' + i))").toBool());
        QCOMPARE(bench.plugins->settingsOf("pinhole.screenshots").value("found:folder:/y").toArray().size(), 5000);

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
        kept = bench.plugins->settingsOf("pinhole.screenshots");
        QVERIFY(!kept.contains("found:"));
        QCOMPARE(QStringList(kept.keys()), (QStringList{"found:folder:/x", "found:folder:/y", "hide"}));
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
        grid.press(QPointF(450, 50));
        grid.release(QPointF(450, 50));
        QCOMPARE(grid.selection(), QList<int>{4});
        grid.press(QPointF(150, 50));
        grid.move(QPointF(250, 350));
        QCOMPARE(grid.selection(), (QList<int>{1, 2, 4}));
        // a hidden meanwhile: the sweep goes on from b, and keeps e, though
        // both are in other cells now.
        grid.setHidden({0});
        QCOMPARE(grid.selection(), (QList<int>{1, 2, 4}));
        grid.move(QPointF(250, 350)); // d's cell now
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

    // Export's JPEG of a HEIC is kept by the file it came from: two of one
    // name in two folders, or one written anew, are not given each other's.
    void aCompatibleJpegIsKeptByItsFileNotItsName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir().mkpath(dir.path() + "/a");
        QDir().mkpath(dir.path() + "/b");
        auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(bytes);
        };
        const QString a = dir.path() + "/a/IMG_0001.HEIC", b = dir.path() + "/b/IMG_0001.HEIC";
        write(a, "one");
        write(b, "one");
        const QString nameA = Outgoing::compatibleName(a);
        QVERIFY(nameA.endsWith(".jpg"));
        QVERIFY(nameA != Outgoing::compatibleName(b));
        // Touched (as pinholed does what it serves): the same.
        QFile(a).setFileTime(QDateTime::currentDateTime().addDays(1), QFileDevice::FileModificationTime);
        QCOMPARE(Outgoing::compatibleName(a), nameA);
        // Written over, or replaced by another file: another.
        write(a, "edited");
        const QString edited = Outgoing::compatibleName(a);
        QVERIFY(edited != nameA);
        write(dir.path() + "/c", "edited");
        QVERIFY(QFile::remove(a));
        QVERIFY(QFile::rename(dir.path() + "/c", a));
        QVERIFY(Outgoing::compatibleName(a) != edited);
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
        // The service's first line, as a service of this window's protocol.
        void greet() { send({{"event", "hello"}, {"protocol", ServiceClient::kProtocol}, {"version", "1.0.0"}}); }
    };

private slots:
    // A favourite made in one source's library, undone once another's of
    // the same ids is shown: not on those, and the service is told whose
    // they are, should it have the other's already.
    void aFavouriteIsUndoneOnlyOnTheSourceItWasMadeIn() {
        QTemporaryDir dir, a, b;
        makeLibrary(a.path(), {{"x.jpg", 1}, {"y.jpg", 2}}, false, "folder:/a");
        makeLibrary(b.path(), {{"x.jpg", 1}, {"y.jpg", 2}}, false, "folder:/b");
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(dir.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();
        Library library, other;
        QString error;
        QVERIFY(library.open(a.path(), &error));
        QVERIFY(other.open(b.path(), &error));
        History history;
        Favourites favourites(&library, &service, &history);
        favourites.toggle({0});
        QVERIFY(library.at(0).favorite);
        const auto heard = [&] {
            fake.read();
            return fake.asked.size();
        };
        QTRY_COMPARE(heard(), 1);
        const QJsonObject asked = fake.asked.first();
        QCOMPARE(asked.value("method").toString(), QString("set_favorite"));
        QCOMPARE(asked.value("params").toObject().value("source").toString(), QString("folder:/a"));
        // Answered as by a service already showing the other.
        fake.send({{"id", asked.value("id")}, {"ok", false}, {"error", QJsonObject{{"code", "other_source"}, {"message", "no"}}}});
        QTRY_VERIFY(!library.at(0).favorite);
        favourites.toggle({0});
        QTRY_COMPARE(heard(), 2);
        library.swap(other);
        QVERIFY(history.canUndo());
        history.undo();
        QTest::qWait(100);
        QCOMPARE(heard(), 2);
        QVERIFY(!library.at(0).favorite);
        // A photo asked for says whose it is too, once the window says.
        service.setShown(library.source());
        service.fetch("x.jpg", "medium", [](bool, const QJsonObject &, const QString &) {});
        QTRY_COMPARE(heard(), 3);
        QCOMPARE(fake.asked.last().value("params").toObject().value("source").toString(), QString("folder:/b"));
    }

    // A library given to the window (--library) is only looked at: F says
    // so, not that the source would not take the change.
    void aFavouriteInALibraryOnlyLookedAtSaysSo() {
        QTemporaryDir dir, a;
        makeLibrary(a.path(), {{"x.jpg", 1}}, false, "folder:/a");
        FakeService fake;
        QVERIFY(fake.server.listen(dir.path() + "/s.sock"));
        qputenv("PINHOLE_SOCKET", (dir.path() + "/s.sock").toUtf8());
        ServiceClient service(a.path());
        QTRY_VERIFY(fake.server.hasPendingConnections());
        fake.peer = fake.server.nextPendingConnection();
        fake.greet();
        Library library;
        QString error;
        QVERIFY(library.open(a.path(), &error));
        History history;
        Favourites favourites(&library, &service, &history);
        QSignalSpy failed(&favourites, &Favourites::failed);
        favourites.toggle({0});
        const auto asked = [&] {
            fake.read();
            return !fake.asked.isEmpty();
        };
        QTRY_VERIFY(asked());
        fake.send({{"id", fake.asked.first().value("id")}, {"ok", false},
                   {"error", QJsonObject{{"code", "read_only"}, {"message", "only read"}}}});
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains("only being looked at"));
        QVERIFY(!library.at(0).favorite);
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
    }

    void aPluginAskingForANewerApiSaysSo() {
        PluginBench bench({{"t", {{"api", 99}}}, {"u", {{"api", Plugins::api}}}});
        QVERIFY(bench.plugins->at("t").value("error").toString().contains("99"));
        QVERIFY(!bench.plugins->at("t").value("enabled").toBool());
        QCOMPARE(bench.plugins->at("u").value("error").toString(), QString());
        QCOMPARE(bench.js("pinhole.api").toInt(), Plugins::api);
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
        fake.greet();
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
