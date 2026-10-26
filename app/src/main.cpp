// pinhole — a fast photo browser for Omarchy: iCloud Photos on demand, or a
// folder of photos on this computer.

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include "bench.h"
#include "framestats.h"
#include "library.h"
#include "photogrid.h"
#include "outgoing.h"
#include "photoprovider.h"
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
    app.setApplicationName("pinhole");
    app.setApplicationVersion("1.0.0");
    app.setDesktopFileName("pinhole");

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
    Storage storage(&service);
    Sources sources(&service);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("photo"), new PhotoProvider);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("viewer", &viewer);
    engine.rootContext()->setContextProperty("outgoing", &outgoing);
    engine.rootContext()->setContextProperty("storage", &storage);
    engine.rootContext()->setContextProperty("sources", &sources);
    engine.rootContext()->setContextProperty("bench", nullptr);
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
    auto *grid = window->findChild<PhotoGrid *>("grid");
    window->installEventFilter(&viewer);
    grid->setSources(&library, &loader, &stats);
    grid->setTouchpadHold(&hold);
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
                              &viewer] {
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
            const qint64 read = took.elapsed();
            QMetaObject::invokeMethod(window, [=, &library, &reloadAgain, &reloading, &reloadLibrary, &toNewest, &loader, &viewer] {
                QElapsedTimer swapping;
                swapping.start();
                const int before = library.count();
                grid->rememberAnchor();
                loader.reload([&] { library.swap(*fresh); });
                grid->libraryReloaded(moved);
                if (std::exchange(toNewest, false))
                    grid->scrollToEnd();
                viewer.libraryReloaded(moved);
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
    // PINHOLE_SELFTEST=settings:<dir>: the settings panel as it opens, as an
    // image in <dir>. With PINHOLE_SELFTEST_LOWER=1 also with the limit
    // stepped two down, and once that is confirmed: it removes photos, so
    // only for a service on a scratch cache.
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("settings:"))) {
        QTimer::singleShot(1200, window, [window, test] {
            QObject *panel = window->findChild<QObject *>(QStringLiteral("settings"));
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
                    QMetaObject::invokeMethod(panel, "confirm");
                    QTimer::singleShot(2500, window, [=] {
                        window->grabWindow().save(dir + "/3-freed.png");
                        QCoreApplication::quit();
                    });
                });
            });
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
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("viewer"))
        runViewerSelfTest(library, viewer);
    if (const QString test = qEnvironmentVariable("PINHOLE_SELFTEST"); test.startsWith(QLatin1String("copytwice:")))
        runCopyTwiceSelfTest(window, grid, viewer, test.mid(10));
    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("mouse"))
        runMouseSelfTest(window, grid, viewer, library);
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
