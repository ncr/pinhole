// pinhole — a fast photo browser for Omarchy: iCloud Photos on demand, or a
// folder of photos on this computer.

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QTimer>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QtMultimedia/private/qplatformmediaintegration_p.h>

#include "bench.h"
#include "framestats.h"
#include "keymap.h"
#include "library.h"
#include "libraryreloader.h"
#include "photogrid.h"
#include "incoming.h"
#include "outgoing.h"
#include "favourites.h"
#include "history.h"
#include "marks.h"
#include "photoprovider.h"
#include "plugins.h"
#include "selftest.h"
#include "serviceclient.h"
#include "sources.h"
#include "storage.h"
#include "viewer.h"
#include "theme.h"
#include "thumbnailloader.h"
#include "translator.h"
#include "touchpadhold.h"

#include <cstdio>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <thread>

namespace {

// A Vulkan driver that offers at least one GPU, asked of the Vulkan loader
// itself. Its few calls are declared here by hand: Vulkan's headers are a
// build dependency Pinhole does not otherwise need.
bool vulkanUsable() {
    void *vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan)
        return false;
    using Proc = void (*)();
    struct InstanceInfo { // VkInstanceCreateInfo, asking for nothing
        int type = 1;     // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
        const void *next = nullptr;
        unsigned flags = 0;
        const void *application = nullptr;
        unsigned layers = 0;
        const char *const *layerNames = nullptr;
        unsigned extensions = 0;
        const char *const *extensionNames = nullptr;
    } info;
    using Create = int (*)(const InstanceInfo *, const void *, void **);
    using Enumerate = int (*)(void *, unsigned *, void *);
    using Destroy = void (*)(void *, const void *);
    bool usable = false;
    const auto proc = reinterpret_cast<Proc (*)(void *, const char *)>(dlsym(vulkan, "vkGetInstanceProcAddr"));
    if (const auto create = proc ? reinterpret_cast<Create>(proc(nullptr, "vkCreateInstance")) : nullptr) {
        void *instance = nullptr;
        if (create(&info, nullptr, &instance) == 0) { // VK_SUCCESS
            unsigned devices = 0;
            const auto enumerate = reinterpret_cast<Enumerate>(proc(instance, "vkEnumeratePhysicalDevices"));
            usable = enumerate && enumerate(instance, &devices, nullptr) == 0 && devices > 0;
            if (const auto destroy = reinterpret_cast<Destroy>(proc(instance, "vkDestroyInstance")))
                destroy(instance, nullptr);
        }
    }
    dlclose(vulkan);
    return usable;
}

} // namespace

int main(int argc, char *argv[]) {
    // On Asahi, Vulkan stalls far less than OpenGL when many small textures
    // are uploaded per frame: zoomed-out fast scrolling went from 96 to
    // 119 fps and the worst frame from 385 to 66 ms. So Vulkan when there is
    // a GPU to run it on; without one (an old GPU, a virtual machine, a
    // driver missing), Qt's own choice, rather than a window that never
    // draws. QSG_RHI_BACKEND still picks one by hand.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND") && vulkanUsable())
        qputenv("QSG_RHI_BACKEND", "vulkan");
    // Thumbnails share atlas pages; at the default size (half the screen,
    // rounded) a page holds only a dozen of them and a full screen would
    // need dozens of draw calls. 4096 pages hold ~70 full-size thumbnails.
    if (qEnvironmentVariableIsEmpty("QSG_ATLAS_WIDTH"))
        qputenv("QSG_ATLAS_WIDTH", "4096");
    if (qEnvironmentVariableIsEmpty("QSG_ATLAS_HEIGHT"))
        qputenv("QSG_ATLAS_HEIGHT", "4096");

    // Not GTK's platform theme, which Omarchy's environment asks for: it
    // costs about 30 ms at start, and Pinhole draws everything itself (its
    // type, colours and folder picker are Omarchy's, not GTK's). Programs it
    // starts still get the desktop's setting.
    const QByteArray platformTheme = qgetenv("QT_QPA_PLATFORMTHEME");
    if (platformTheme.startsWith("gtk"))
        qunsetenv("QT_QPA_PLATFORMTHEME");
    QGuiApplication app(argc, argv);
    if (platformTheme.startsWith("gtk"))
        qputenv("QT_QPA_PLATFORMTHEME", platformTheme);

    // Qt Multimedia, when first used, tries every hardware video decoder
    // FFmpeg knows of (CUDA, VA-API, VDPAU, Vulkan): some 900 ms on an
    // NVIDIA card, most of the time the window took to open. So it starts
    // here, on a thread of its own, and the viewer, the one thing that
    // plays video, is made once it is ready (mediaReady in Main.qml).
    bool mediaReady = false;
    QQuickWindow *mainWindow = nullptr;
    std::jthread media([&app, &mediaReady, &mainWindow] {
        QPlatformMediaIntegration *integration = QPlatformMediaIntegration::instance();
        // Its later children (the devices) are made on the window's thread.
        integration->moveToThread(app.thread());
        QMetaObject::invokeMethod(&app, [&mediaReady, &mainWindow] {
            mediaReady = true;
            if (mainWindow)
                mainWindow->setProperty("mediaReady", true);
        }, Qt::QueuedConnection);
    });

    app.setApplicationName("pinhole");
    app.setApplicationVersion("1.0.0");
    // Matches the window to pinhole.desktop, so the compositor (its Wayland
    // app id) and Omarchy's launcher pick up the installed icon; the window's
    // own icon is that one too.
    app.setDesktopFileName("pinhole");
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("pinhole")));

    // Where the source's library is: in the folder Pinhole keeps everything
    // in, iCloud's or a folder's as the settings say. The synthetic one for
    // working on the grid (pinhole-fake) is opened with --library.
    const QString defaultLibrary = Storage::library();

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"library", "Library directory (library.sqlite and thumbs/).", "dir", defaultLibrary});
    parser.addOption({"bench", "Run the scripted benchmark, print the results and quit."});
    parser.addOption({"decoders", "Thumbnail decoding threads.", "n",
                      QString::number(std::max(2, QThread::idealThreadCount() - 4))});
    parser.process(app);

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    // PINHOLE_LANGUAGE_FILE: another file than Omarchy's, for trying a
    // language without changing the desktop's.
    Translator translator(qEnvironmentVariable("PINHOLE_LANGUAGE_FILE"));
    app.installTranslator(&translator);
    // PINHOLE_THEME_DIR: another Omarchy "current" folder (one holding
    // theme/colors.toml), to see a theme without switching the desktop's.
    Theme theme(qEnvironmentVariable("PINHOLE_THEME_DIR"));

    Library library;
    // Where the library is read from; it changes if the service moves it.
    QString libraryDir = parser.value("library");
    QString error;
    QElapsedTimer loadTimer;
    loadTimer.start();
    // Read while the window loads (both take some 50 ms): nothing touches
    // the library until opening.join() below. No library yet is not fatal:
    // the window opens and says how to make one, and picks it up once it is
    // there.
    bool haveLibrary = false;
    std::thread opening([&] { haveLibrary = library.open(libraryDir, &error); });

    qmlRegisterType<PhotoGrid>("Pinhole", 1, 0, "PhotoGrid");
    // What a plugin's QML imports (docs/plugins.md): its root and its
    // buttons, and Pinhole's own pieces to draw as Pinhole does.
    for (const char *type : {"PinholePlugin", "PluginAction", "Command", "ShellButton", "Chip", "UiText", "Spinner", "Toggle"})
        qmlRegisterType(QUrl(QStringLiteral("qrc:/%1.qml").arg(QLatin1String(type))), "Pinhole.Plugin", 1, 0, type);
    FrameStats stats;
    ThumbnailLoader loader(&library, parser.value("decoders").toInt());
    // Before the window, so it outlives the grid that polls it.
    TouchpadHold hold;

    // Larger renditions come from pinholed.
    // A library given (pinhole-fake's, say) is the one shown, whatever the
    // service says; else the service's choice is (library_moved).
    const bool libraryGiven = parser.isSet("library");
    ServiceClient service(libraryGiven ? QDir(libraryDir).absolutePath() : QString());
    // What to call the source until the service says.
    service.setSource(Storage::configuredSource());
    Viewer viewer(&library, &service);
    Outgoing outgoing(&library, &service);
    History history;
    Favourites favourites(&library, &service, &history);
    QObject::connect(&favourites, &Favourites::changed, &viewer, &Viewer::favoriteChanged);
    Incoming incoming(&service);
    Storage storage(&service);
    Sources sources(&service);
    Plugins plugins(&library, &service, &viewer, &translator, &history);
    // What was done to one source's photos is not undone on another's.
    QObject::connect(&sources, &Sources::switched, &history, &History::clear);

    Keymap keymap;
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("photo"), new PhotoProvider);
    engine.addImageProvider(QStringLiteral("mark"), new MarkProvider);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    // Every action and its keys, the layer that has them, and how the
    // window was last used (keymap.h).
    engine.rootContext()->setContextProperty("keymap", &keymap);
    engine.rootContext()->setContextProperty("viewer", &viewer);
    engine.rootContext()->setContextProperty("outgoing", &outgoing);
    engine.rootContext()->setContextProperty("favourites", &favourites);
    engine.rootContext()->setContextProperty("history", &history);
    engine.rootContext()->setContextProperty("incoming", &incoming);
    engine.rootContext()->setContextProperty("storage", &storage);
    engine.rootContext()->setContextProperty("sources", &sources);
    engine.rootContext()->setContextProperty("plugins", &plugins);
    plugins.setEngine(&engine);
    engine.rootContext()->setContextProperty("bench", nullptr);
    // Videos play without sound in test runs (a selftest, the bench, or
    // PINHOLE_SILENT=1), so a window on a hidden workspace makes no noise.
    engine.rootContext()->setContextProperty("silent", qEnvironmentVariableIsSet("PINHOLE_SELFTEST")
                                                           || parser.isSet("bench")
                                                           || qEnvironmentVariable("PINHOLE_SILENT") == QLatin1String("1"));
    // A plugin's tour shows by itself the first time (Tour.qml), but not over
    // a self-test or the bench, unless it is the tour's own test.
    engine.rootContext()->setContextProperty("autoTours", (!qEnvironmentVariableIsSet("PINHOLE_SELFTEST")
                                                           || qEnvironmentVariable("PINHOLE_SELFTEST").startsWith(QLatin1String("tour:")))
                                                          && !parser.isSet("bench"));
    // The line to pinholed, for how the source is doing (service.health).
    engine.rootContext()->setContextProperty("service", &service);
    engine.load(QUrl("qrc:/Main.qml"));
    opening.join();
    if (!haveLibrary)
        std::fprintf(stderr, "pinhole: %s\n", qPrintable(error));
    std::fprintf(stderr, "pinhole: %d items from %s in %lld ms\n", library.count(), qPrintable(library.dir()),
                 loadTimer.elapsed());
    if (engine.rootObjects().isEmpty())
        return 1;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    mainWindow = window;
    window->setProperty("mediaReady", mediaReady);
    auto *grid = window->findChild<PhotoGrid *>("grid");
    window->installEventFilter(&viewer);
    grid->setSources(&library, &loader, &stats);
    plugins.setGrid(grid);
    grid->setTouchpadHold(&hold);
    // A favourite's mark, the moment it is set.
    QObject::connect(&favourites, &Favourites::changed, grid, [grid] { grid->update(); });
    // A resting finger stops a zoomed photo coasting as well.
    QObject::connect(&hold, &TouchpadHold::began, &viewer, [&viewer](quint32 time, quint32) { viewer.touch(time); });
    QObject::connect(grid, &PhotoGrid::dragRequested, &outgoing,
                     [&outgoing, grid](const QList<int> &indexes) { outgoing.drag(grid, indexes); });

    // Medium requests arrive from the render thread; the connection queues
    // them onto this one. Positions are those of the generation they were
    // asked in: a reload since, and they are dropped (and asked again).
    QObject::connect(&loader, &ThumbnailLoader::mediumNeeded, &service, [&](const QList<int> &indexes, quint64 generation) {
        if (generation != loader.generation())
            return;
        for (int index : indexes) {
            service.fetch(library.at(index).id, QStringLiteral("medium"),
                          [&loader, grid, index, generation](bool ok, const QJsonObject &result, const QString &code) {
                              if (ok) {
                                  loader.setMediumPath(index, QFile::encodeName(result.value("path").toString()), generation);
                                  grid->update();
                                  return;
                              }
                              loader.setMediumUnavailable(index, generation);
                              // Missing for good, or only for now: try again later.
                              if (code != QLatin1String("not_found"))
                                  QTimer::singleShot(30000, &loader, [&loader, index, generation] { loader.clearMedium(index, generation); });
                          });
        }
    });
    QObject::connect(window, &QQuickWindow::beforeSynchronizing, window, [&stats] { stats.syncBegan(); },
                     Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::beforeRendering, window, [&stats] { stats.renderBegan(); },
                     Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::afterRendering, window, [&stats] { stats.renderEnded(); },
                     Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::frameSwapped, window, [&stats] { stats.frameSwapped(); },
                     Qt::DirectConnection);

    // The service found changes in the source: read the library again and
    // let everything that holds positions follow its photos.
    LibraryReloader reloader(&library, &loader, libraryDir);
    QObject::connect(&reloader, &LibraryReloader::aboutToSwap, grid, [grid] { grid->rememberAnchor(); });
    QObject::connect(&reloader, &LibraryReloader::reloaded, grid,
                     [&, grid](const std::vector<int> &moved, const std::vector<int> &textures, bool toNewest) {
        grid->libraryReloaded(moved, &textures);
        if (toNewest)
            grid->scrollToEnd();
        viewer.libraryReloaded(moved);
        plugins.libraryReloaded();
        favourites.reloaded();
    });
    // Undone on another source's photos of the same ids, a step would
    // change photos it was never done to.
    QObject::connect(&reloader, &LibraryReloader::otherSource, &history, &History::clear);
    // What the service says that the window as a whole follows.
    QObject::connect(&service, &ServiceClient::announced, &service, [&](const QString &name, const QJsonObject &details) {
        if (name == QLatin1String("library_changed")) {
            reloader.reload();
        } else if (name == QLatin1String("source")) {
            // Another source than the one shown (switched to from here or
            // from elsewhere): its library comes next, and nothing done to
            // these photos is undone on its photos of the same ids, even
            // before it is read.
            if (details.value("id").toString() != library.source()) {
                reloader.showNewestNext();
                history.clear();
            }
        } else if (name == QLatin1String("library_moved") && !libraryGiven) {
            // Read from its new place from now on (the old one goes soon),
            // or, as the window connects, the one the service chose.
            reloader.moveTo(details.value("library").toString());
        }
    });
    // New photos since the window last looked: ask now rather than within
    // the minute.
    service.sync();

    // Without a library, look for one: pinhole-sync may be building it
    // right now.
    if (!haveLibrary)
        reloader.waitForLibrary();

    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST");
        !test.isEmpty() && !runSelfTest(test, {window, grid, library, viewer, incoming, outgoing, keymap, reloader}))
        return 2;

    Bench bench(grid, &stats, &loader);
    if (parser.isSet("bench")) {
        engine.rootContext()->setContextProperty("bench", &bench);
        QObject::connect(&bench, &Bench::finished, &app, &QCoreApplication::quit);
        // Give the window a moment to be mapped and sized by the compositor.
        QTimer::singleShot(1500, &bench, &Bench::start);
    }

    return app.exec();
}
