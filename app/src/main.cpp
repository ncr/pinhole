// pinhole — a fast, on-demand iCloud Photos browser for Omarchy.

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
#include <QThread>
#include <QUrl>

#include "bench.h"
#include "framestats.h"
#include "library.h"
#include "photogrid.h"
#include "photoprovider.h"
#include "selftest.h"
#include "serviceclient.h"
#include "viewer.h"
#include "thumbnailloader.h"
#include "touchpadhold.h"

#include <cstdio>

int main(int argc, char *argv[]) {
    // On Asahi, Vulkan stalls far less than OpenGL when many small textures
    // are uploaded per frame: zoomed-out fast scrolling went from 96 to
    // 119 fps and the worst frame from 385 to 66 ms.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND"))
        qputenv("QSG_RHI_BACKEND", "vulkan");
    // Thumbnails share atlas pages; at the default size (half the screen,
    // rounded) a page holds only a dozen of them and a full screen would
    // need dozens of draw calls. 4096 pages hold ~70 full-size thumbnails.
    if (qEnvironmentVariableIsEmpty("QSG_ATLAS_WIDTH"))
        qputenv("QSG_ATLAS_WIDTH", "4096");
    if (qEnvironmentVariableIsEmpty("QSG_ATLAS_HEIGHT"))
        qputenv("QSG_ATLAS_HEIGHT", "4096");

    QGuiApplication app(argc, argv);
    app.setApplicationName("pinhole");
    app.setApplicationVersion("0.1.0");
    app.setDesktopFileName("pinhole");

    // The real library once pinhole-sync has made one, the synthetic one
    // otherwise.
    const QString data = QDir::homePath() + "/.local/share/pinhole/";
    const QString defaultLibrary =
        QFile::exists(data + "library/library.sqlite") ? data + "library" : data + "fake";

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"library", "Library directory (library.sqlite and thumbs/).", "dir", defaultLibrary});
    parser.addOption({"bench", "Run the scripted benchmark, print the results and quit."});
    parser.addOption({"decoders", "Thumbnail decoding threads.", "n",
                      QString::number(std::max(2, QThread::idealThreadCount() - 4))});
    parser.process(app);

    QQuickStyle::setStyle("Material");

    Library library;
    QString error;
    QElapsedTimer loadTimer;
    loadTimer.start();
    if (!library.open(parser.value("library"), &error)) {
        std::fprintf(stderr, "pinhole: %s\n", qPrintable(error));
        return 1;
    }
    std::fprintf(stderr, "pinhole: %d items from %s in %lld ms\n", library.count(), qPrintable(library.dir()),
                 loadTimer.elapsed());

    qmlRegisterType<PhotoGrid>("Pinhole", 1, 0, "PhotoGrid");
    FrameStats stats;
    ThumbnailLoader loader(&library, parser.value("decoders").toInt());
    // Before the window, so it outlives the grid that polls it.
    TouchpadHold hold;

    // Larger renditions come from pinholed.
    ServiceClient service;
    Viewer viewer(&library, &service);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("photo"), new PhotoProvider);
    engine.rootContext()->setContextProperty("viewer", &viewer);
    engine.rootContext()->setContextProperty("bench", nullptr);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto *grid = window->findChild<PhotoGrid *>("grid");
    grid->setSources(&library, &loader, &stats);
    grid->setTouchpadHold(&hold);

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

    if (qEnvironmentVariable("PINHOLE_SELFTEST") == QLatin1String("viewer"))
        runViewerSelfTest(library, viewer);

    Bench bench(grid, &stats, &loader);
    if (parser.isSet("bench")) {
        engine.rootContext()->setContextProperty("bench", &bench);
        QObject::connect(&bench, &Bench::finished, &app, &QCoreApplication::quit);
        // Give the window a moment to be mapped and sized by the compositor.
        QTimer::singleShot(1500, &bench, &Bench::start);
    }

    return app.exec();
}
