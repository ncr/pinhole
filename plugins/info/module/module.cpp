#include "maptiles.h"

#include <QQmlExtensionPlugin>
#include <qqml.h>

// Pinhole.Internal.Info: what Info's map draws with, which QML could not do
// fast enough. Only Pinhole's own plugins import Pinhole.Internal.
class InfoModule : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)
public:
    void registerTypes(const char *uri) override {
        qmlRegisterType<MapTiles>(uri, 1, 0, "MapTiles");
        qmlRegisterType<MapWheel>(uri, 1, 0, "MapWheel");
        qmlRegisterType(QUrl(QStringLiteral("qrc:/pinhole-info/MapGestures.qml")), uri, 1, 0, "MapGestures");
    }
};

#include "module.moc"
