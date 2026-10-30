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
#include <QWheelEvent>
#include <QtMultimedia/private/qplatformmediaintegration_p.h>

#include "bench.h"
#include "framestats.h"
#include "keymap.h"
#include "library.h"
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
    ServiceClient service(QDir(libraryDir).absolutePath());
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
    // How the source is doing (set again as the service says): see below.
    engine.rootContext()->setContextProperty("serviceHealth", QStringLiteral("fine"));
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
    // them onto this one.
    QObject::connect(&loader, &ThumbnailLoader::mediumNeeded, &service, [&](const QList<int> &indexes) {
        for (int index : indexes) {
            service.fetch(library.at(index).id, QStringLiteral("medium"),
                          [&loader, grid, index](bool ok, const QJsonObject &result, const QString &code) {
                              if (ok) {
                                  loader.setMediumPath(index, QFile::encodeName(result.value("path").toString()));
                                  grid->update();
                                  return;
                              }
                              loader.setMediumUnavailable(index);
                              // Missing for good, or only for now: try again later.
                              if (code != QLatin1String("not_found"))
                                  QTimer::singleShot(30000, &loader, [&loader, index] { loader.clearMedium(index); });
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

    // The service found changes in the source: read the library again, swap it in
    // with the decoders idle, and let everything that holds positions follow
    // its photos.
    // Reading the index and matching old positions to new takes ~100 ms at
    // 100,000 photos, so it happens on a thread of its own; the window only
    // stops for the swap. A change announced meanwhile reloads once more.
    std::thread reader;
    bool reloading = false, reloadAgain = false, toNewest = false;
    std::function<void()> reloadLibrary = [&] {
        if (reloading) {
            reloadAgain = true;
            return;
        }
        reloading = true;
        if (reader.joinable())
            reader.join();
        reader = std::thread([&library, dir = libraryDir, window, &reloadAgain, &reloading, &reloadLibrary, &toNewest, &loader, grid,
                              &viewer, &plugins, &favourites] {
            QElapsedTimer took;
            took.start();
            auto fresh = std::make_shared<Library>();
            QString error;
            if (!fresh->open(dir, &error)) {
                std::fprintf(stderr, "pinhole: reloading the library: %s\n", qPrintable(error));
                QMetaObject::invokeMethod(window, [&reloading] { reloading = false; }, Qt::QueuedConnection);
                return;
            }
            // Only the window's thread swaps the library, and not before
            // this hands over, so reading the old one here is safe.
            QHash<QByteArray, int> positions;
            positions.reserve(fresh->count());
            for (int i = 0; i < fresh->count(); ++i)
                positions.insert(fresh->at(i).id, i);
            std::vector<int> moved(size_t(library.count()), -1);
            for (int i = 0; i < library.count(); ++i)
                moved[size_t(i)] = positions.value(library.at(i).id, -1);
            // A photo whose thumbnail is a new file is drawn afresh.
            std::vector<int> textures = moved;
            for (int i = 0; i < library.count(); ++i)
                if (const int now = moved[size_t(i)]; now >= 0 && fresh->at(now).thumbPath != library.at(i).thumbPath)
                    textures[size_t(i)] = -1;
            const qint64 read = took.elapsed();
            QMetaObject::invokeMethod(window, [=, &library, &reloadAgain, &reloading, &reloadLibrary, &toNewest, &loader, &viewer, &plugins, &favourites] {
                QElapsedTimer swapping;
                swapping.start();
                const int before = library.count();
                grid->rememberAnchor();
                loader.reload([&] { library.swap(*fresh); });
                grid->libraryReloaded(moved, &textures);
                if (std::exchange(toNewest, false))
                    grid->scrollToEnd();
                viewer.libraryReloaded(moved);
                plugins.libraryReloaded();
                favourites.reloaded();
                std::fprintf(stderr, "pinhole: library reloaded: %d items (was %d); read in %lld ms, swapped in %lld ms\n",
                             library.count(), before, read, swapping.elapsed());
                reloading = false;
                if (std::exchange(reloadAgain, false))
                    reloadLibrary();
            }, Qt::QueuedConnection);
        });
    };
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&reader] {
        if (reader.joinable())
            reader.join();
    });
    // How the source is doing, as the service sees it: fine, needs_sign_in,
    // offline, failing, or no_service when there is no service to ask.
    QObject::connect(&service, &ServiceClient::announced, &service, [&](const QString &name, const QJsonObject &details) {
        if (name == QLatin1String("library_changed")) {
            reloadLibrary();
        } else if (name == QLatin1String("source")) {
            // Another source's library comes next: shown from its newest
            // photos, not from where the last one was.
            toNewest = true;
        } else if (name == QLatin1String("library_moved")) {
            // Read from its new place from now on (the old one goes soon).
            libraryDir = details.value("library").toString();
            service.setLibrary(libraryDir);
            reloadLibrary();
        }
        else if (name == QLatin1String("health"))
            engine.rootContext()->setContextProperty("serviceHealth", details.value("state").toString());
    });
    // New photos since the window last looked: ask now rather than within
    // the minute.
    service.sync();

    // Without a library, look for one every few seconds: pinhole-sync may be
    // building it right now.
    QTimer waitForLibrary;
    if (!haveLibrary) {
        QObject::connect(&waitForLibrary, &QTimer::timeout, &waitForLibrary, [&] {
            if (!QFile::exists(libraryDir + "/library.sqlite"))
                return;
            waitForLibrary.stop();
            reloadLibrary();
        });
        waitForLibrary.start(5000);
    }

    // PINHOLE_SELFTEST=sourcesfade:<dir>: the side panel coming in and going
    // again, the photos dimming behind it and the grid's controls fading, as
    // frames in <dir> (on screen, for the photos to be drawn).
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("sourcesfade:"))) {
        QTimer::singleShot(2500, window, [window, test] {
            QObject *bar = window->findChild<QObject *>(QStringLiteral("sidePanel"));
            const QString dir = test.mid(12);
            auto grab = [window, dir](const QString &name) { window->grabWindow().save(dir + '/' + name + ".png"); };
            grab("0-before");
            // Apart, as a grab on screen comes back with a frame from a
            // moment later.
            QTimer::singleShot(500, window, [=] { bar->setProperty("shown", true); });
            QTimer::singleShot(560, window, [=] { grab("1-coming"); });
            QTimer::singleShot(1000, window, [=] {
                grab("2-shown");
                bar->setProperty("shown", false);
                QTimer::singleShot(80, window, [=] { grab("3-going"); });
                QTimer::singleShot(400, window, [=] {
                    grab("4-after");
                    QCoreApplication::quit();
                });
            });
        });
    }
    // PINHOLE_SELFTEST=help:<file>: the shortcut overlay as an image, to check
    // its layout (and each language's) without a screen.
    // PINHOLE_SELFTEST=shot:<file>: the window as it first shows, as an image.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("shot:"))) {
        // PINHOLE_SELFTEST_DELAY: how long to wait first, in ms.
        const int delay = qEnvironmentVariableIsSet("PINHOLE_SELFTEST_DELAY") ? qEnvironmentVariableIntValue("PINHOLE_SELFTEST_DELAY") : 1200;
        QTimer::singleShot(delay, window, [window, test] {
            window->grabWindow().save(test.mid(5));
            QCoreApplication::quit();
        });
    }
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("help:"))) {
        // PINHOLE_SELFTEST_SIZE: the window's, as 1400x900.
        if (const QStringList size = qEnvironmentVariable("PINHOLE_SELFTEST_SIZE").split('x'); size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        QTimer::singleShot(800, window, [window, test] {
            if (QObject *help = window->findChild<QObject *>(QStringLiteral("help")))
                help->setProperty("shown", true);
            QTimer::singleShot(400, window, [window, test] {
                window->grabWindow().save(test.mid(5));
                QCoreApplication::quit();
            });
        });
    }
    // PINHOLE_SELFTEST=selectbar:<dir>: select mode's bar with three photos
    // ticked, at window widths from wide to a phone's, as images in <dir>.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("selectbar:"))) {
        QTimer::singleShot(1200, window, [window, grid, test] {
            grid->setSelecting(true);
            grid->selectAll();
            grid->jumpToFraction(0.5); // away from the newest: the Newest button shows
            auto widths = std::make_shared<QList<int>>(QList<int>{1400, 1000, 760, 560, 420, 360});
            auto next = std::make_shared<std::function<void()>>();
            *next = [window, grid, test, widths, next] {
                if (widths->isEmpty()) {
                    QCoreApplication::quit();
                    return;
                }
                const int width = widths->takeFirst();
                window->resize(width, 600);
                QTimer::singleShot(300, window, [window, test, width, next] {
                    window->grabWindow().copy(0, 440, width, 160).save(QStringLiteral("%1/bar-%2.png").arg(test.mid(10)).arg(width));
                    (*next)();
                });
            };
            (*next)();
        });
    }
    // PINHOLE_SELFTEST=settings:<dir>: the side panel as it opens, as an
    // image in <dir>. With PINHOLE_SELFTEST_LOWER=1 also with the limit
    // stepped two down, and once that is confirmed: it removes photos, so
    // only for a service on a scratch cache.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("settings:"))) {
        QTimer::singleShot(1200, window, [window, test] {
            QObject *panel = window->findChild<QObject *>(QStringLiteral("sidePanel"));
            const QString dir = test.mid(9);
            panel->setProperty("shown", true);
            QTimer::singleShot(1000, window, [=] {
                window->grabWindow().save(dir + "/1-open.png");
                if (qEnvironmentVariable("PINHOLE_SELFTEST_LOWER") != QLatin1String("1")) {
                    QCoreApplication::quit();
                    return;
                }
                QMetaObject::invokeMethod(panel, "step", Q_ARG(QVariant, -1));
                QMetaObject::invokeMethod(panel, "step", Q_ARG(QVariant, -1));
                QTimer::singleShot(800, window, [=] {
                    window->grabWindow().save(dir + "/2-lower.png");
                    QMetaObject::invokeMethod(panel, "confirmLimit");
                    QTimer::singleShot(2500, window, [=] {
                        window->grabWindow().save(dir + "/3-freed.png");
                        QCoreApplication::quit();
                    });
                });
            });
        });
    }
    // PINHOLE_SELFTEST=sourcesbar:<dir>: the side panel as it opens and a
    // few seconds and a round later, as images in <dir>, to see when the
    // source shown was looked at count on.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("sourcesbar:"))) {
        QTimer::singleShot(1500, window, [window, test] {
            window->findChild<QObject *>(QStringLiteral("sidePanel"))->setProperty("shown", true);
            const QString dir = test.mid(11);
            for (const int seconds : {1, 4, 13}) {
                QTimer::singleShot(seconds * 1000, window, [=] {
                    window->grabWindow().save(dir + QStringLiteral("/after-%1s.png").arg(seconds));
                    if (seconds == 13)
                        QCoreApplication::quit();
                });
            }
        });
    }
    // PINHOLE_SELFTEST=zoomfast:<dir>: a quick pinch from 3 columns all the
    // way out, the date label's state every 16 ms on the output and as
    // frames of the top left in <dir>.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("zoomfast:"))) {
        QTimer::singleShot(1200, window, [window, grid, test] {
            grid->setColumnsNow(3);
            grid->jumpToFraction(0.6);
            auto *clock = new QElapsedTimer;
            auto *frame = new int(0);
            auto *timer = new QTimer(window);
            QObject *label = window->findChild<QObject *>(QStringLiteral("dateLabel"));
            QTimer::singleShot(500, window, [=] {
                clock->start();
                QObject::connect(timer, &QTimer::timeout, window, [=] {
                    const double t = clock->nsecsElapsed() / 1e9;
                    const QPointF middle(grid->width() / 2, grid->height() / 2);
                    if (t < 0.5)
                        grid->setEffectiveStride(grid->width() / (3 * std::pow(29.0 / 3, t / 0.5)), middle);
                    else if (!grid->property("ended").toBool()) {
                        grid->setProperty("ended", true);
                        grid->endContinuousZoom();
                    } else if (t > 1.5) {
                        timer->stop();
                        QCoreApplication::quit();
                    }
                    std::printf("%03d %5.3f grid=%-28s shown=%-28s morphed=%.2f ticking=%d w=%.1f\n", *frame, t,
                                qPrintable(grid->dateLabel()), qPrintable(label->property("shownText").toString()),
                                label->property("morphed").toDouble(), label->property("ticking").toBool(),
                                label->property("width").toDouble());
                    std::fflush(stdout);
                    window->grabWindow().copy(0, 0, 420, 70).save(QStringLiteral("%1/%2.png").arg(test.mid(9)).arg(*frame, 3, 10, QLatin1Char('0')));
                    ++*frame;
                });
                timer->start(16);
            });
        });
    }
    // PINHOLE_SELFTEST=wheelzoom: a mouse's horizontal wheel turned a little
    // at a time from 7 columns, a second apart, and the level each turn lands
    // on, on the output: never back where it began unless it hardly turned,
    // and never more than a level, even for a burst of notches (as an MX
    // Master's thumb wheel sends for the slightest touch).
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("wheelzoom")) {
        QTimer::singleShot(1200, window, [window, grid] {
            grid->setColumnsNow(7);
            grid->jumpToFraction(0.5);
            // Frames stepped here too, in case the window is hidden and draws none.
            auto *frames = new QTimer(window);
            QObject::connect(frames, &QTimer::timeout, window, [grid] { grid->advance(1.0 / 60); });
            frames->start(16);
            const std::vector<std::vector<int>> turns = {{24}, {120}, {-120}, {-120}, {-120}, {120, 120, 120}, {60}, {-24},
                                                       {-120, -120, -120, -120, -120, -120, -120, -120}, {360, 360}};
            for (size_t k = 0; k <= turns.size(); ++k) {
                QTimer::singleShot(int(k) * 1000 + 500, window, [grid, turns, k] {
                    if (k > 0)
                        std::printf("turn %zu: lands on %d columns\n", k, grid->columns()), std::fflush(stdout);
                    if (k == turns.size())
                        return QCoreApplication::quit();
                    for (size_t n = 0; n < turns[k].size(); ++n)
                        QTimer::singleShot(int(n) * 30, grid, [grid, x = turns[k][n]] {
                            const QPointF at(grid->width() / 2, grid->height() / 2);
                            QWheelEvent event(at, grid->mapToGlobal(at), QPoint(), QPoint(x, 0), Qt::NoButton, Qt::NoModifier,
                                              Qt::NoScrollPhase, false);
                            QCoreApplication::sendEvent(grid, &event);
                        });
                });
            }
        });
    }
    // PINHOLE_SELFTEST=zoomdates:<dir>: the date label through a pinch from
    // 7 columns out to 29 and in to 3, then two seconds of scrolling at 7
    // (where the label is a range of days), as frames of the top left in
    // <dir>, with the label of each on the output.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("zoomdates:"))) {
        QTimer::singleShot(1200, window, [window, grid, test] {
            grid->setColumnsNow(7);
            grid->jumpToFraction(0.5);
            auto *timer = new QTimer(window);
            auto *frame = new int(0);
            QObject::connect(timer, &QTimer::timeout, window, [window, grid, test, timer, frame] {
                const double t = *frame * 0.05;
                auto lerpLog = [](double a, double b, double f) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * f); };
                if (t < 2.0)
                    grid->setEffectiveStride(grid->width() / lerpLog(7, 29, t / 2.0), QPointF(grid->width() / 2, grid->height() / 2));
                else if (t < 4.0)
                    grid->setEffectiveStride(grid->width() / lerpLog(29, 3, (t - 2.0) / 2.0), QPointF(grid->width() / 2, grid->height() / 2));
                else if (t < 4.05)
                    grid->endContinuousZoom();
                // Then scrolling up through the days, the label changing as
                // it goes.
                else if (t >= 4.6 && t < 4.65)
                    grid->setColumnsNow(7), grid->jumpToFraction(0.97), grid->setDriveVelocity(-6000);
                else if (t >= 6.6 && t < 6.65)
                    grid->setDriveVelocity(0);
                else if (t > 7.0) {
                    timer->stop();
                    QCoreApplication::quit();
                }
                std::printf("%03d %s %.4f %s\n", *frame, grid->zooming() ? "zooming" : "still  ", grid->scrollFraction(),
                            grid->dateLabel().isEmpty() ? "-" : qPrintable(grid->dateLabel()));
                std::fflush(stdout);
                window->grabWindow().copy(0, 0, 420, 70).save(QStringLiteral("%1/%2.png").arg(test.mid(10)).arg(*frame, 3, 10, QLatin1Char('0')));
                ++*frame;
            });
            timer->start(50);
        });
    }
    // PINHOLE_SELFTEST=neon:<dir>: zooming out to 29 columns, where the
    // marks' tubes strike, in and out again, then in to 7, where they are
    // white, and out to 13, where the photos dim and the tubes strike anew,
    // as 60 frames a second in <dir>
    // (PINHOLE_SELFTEST_AT: where in the library). Frames are stepped here,
    // not by the clock, so slow grabs do not change the timing. 29 first, so
    // its thumbnails are in.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("neon:"))) {
        QTimer::singleShot(2500, window, [window, grid, test] {
            const double at = qEnvironmentVariable("PINHOLE_SELFTEST_AT").toDouble();
            window->setProperty("troubleSeen", true);
            grid->setColumnsNow(29);
            grid->jumpToFraction(at);
            QTimer::singleShot(5000, window, [grid, at] { grid->setColumnsNow(13), grid->jumpToFraction(at); });
            QTimer::singleShot(8000, window, [window, grid, test] {
                int frame = 0;
                auto frames = [&](int n) {
                    for (int i = 0; i < n; ++i, ++frame) {
                        grid->advance(1.0 / 60);
                        window->grabWindow().save(QStringLiteral("%1/%2.jpg").arg(test.mid(5)).arg(frame, 4, 10, QLatin1Char('0')), nullptr, 92);
                    }
                };
                frames(30);
                grid->zoomStep(1);
                frames(150);
                grid->zoomStep(-1);
                frames(50);
                grid->zoomStep(1);
                frames(150);
                grid->zoomStep(-1);
                frames(50);
                grid->zoomStep(-1);
                frames(50);
                grid->zoomStep(1);
                frames(150);
                QCoreApplication::quit();
            });
        });
    }
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("drop:")))
        runDropSelfTest(window, library, incoming, test.mid(5), qEnvironmentVariable("PINHOLE_SELFTEST_DROP").split(':', Qt::SkipEmptyParts));
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("viewer"))
        runViewerSelfTest(library, viewer);
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("copytwice:")))
        runCopyTwiceSelfTest(window, grid, viewer, test.mid(10));
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("live:")))
        runLiveSelfTest(window, library, viewer, test.mid(5));
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("actionbar:")))
        runActionBarSelfTest(window, grid, viewer, library, test.mid(10));
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("favourite:")))
        runFavouriteSelfTest(window, grid, viewer, library, test.mid(10));
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("tour:")))
        runTourSelfTest(window, viewer, test.mid(5));
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("series:")))
        runSeriesSelfTest(window, viewer, test.mid(7));
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("undo:")))
        runUndoSelfTest(window, viewer, test.mid(5));
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("mouse"))
        runMouseSelfTest(window, grid, viewer, library);
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("keys"))
        runKeysSelfTest(window, grid, viewer, keymap);
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("reload"))
        runReloadSelfTest(library, grid, viewer);
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("outgoing"))
        runOutgoingSelfTest(library, outgoing, qEnvironmentVariable("PINHOLE_SELFTEST_DIR"));

    Bench bench(grid, &stats, &loader);
    if (parser.isSet("bench")) {
        engine.rootContext()->setContextProperty("bench", &bench);
        QObject::connect(&bench, &Bench::finished, &app, &QCoreApplication::quit);
        // Give the window a moment to be mapped and sized by the compositor.
        QTimer::singleShot(1500, &bench, &Bench::start);
    }

    return app.exec();
}
