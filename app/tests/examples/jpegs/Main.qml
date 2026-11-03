import QtQuick
import Pinhole.Plugin

// Add a JPEG folder through the source picker; nothing runs during startup.
PinholePlugin {
    id: plugin

    function choose() {
        pinhole.chooseFolder(qsTr("Choose a JPEG folder"))
            .then(path => pinhole.sources.add("me.jpegs", { folder: path }))
            .catch(error => { if (error.code !== "cancelled") pinhole.warn(error) })
    }

    sources: [PluginSource {
        kind: "me.jpegs"
        noun: qsTr("JPEG folder")
        place: source => source.name
        ways: [{ title: qsTr("JPEG folder…"), key: "J", triggered: () => plugin.choose() }]
    }]
}
