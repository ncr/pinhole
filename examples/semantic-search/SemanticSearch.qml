import QtQuick
import Pinhole.Plugin

// Indexing and the on-demand search view have independent lifetimes.
PinholePlugin {
    id: plugin
    covering: view.item !== null && view.item.shown
    actions: [PluginAction {
        place: "grid"
        text: qsTr("Search photos")
        key: "Ctrl+F"
        shown: pinhole.library.count > 0
        onTriggered: {
            view.active = true
            view.item.show()
        }
    }]
    help: [["Ctrl+F", qsTr("Search photo content")]]
    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 6
            Component.onCompleted: backgroundIndex.ensureStarted()
            SettingNote {
                text: backgroundIndex.summary
            }
            SettingNote {
                dim: true
                visible: text !== ""
                text: backgroundIndex.profile?.name || ""
            }
            SettingSwitch {
                text: qsTr("Index new photos")
                usable: backgroundIndex.catalogReady && !backgroundIndex.indexingJob
                checked: !backgroundIndex.paused
                onToggled: backgroundIndex.toggleIndexing()
            }
            ShellButton {
                text: qsTr("Use the 384-photo benchmark")
                visible: Boolean(backgroundIndex.profile?.benchmark)
                usable: backgroundIndex.catalogReady && !backgroundIndex.status.running
                onTapped: backgroundIndex.importBenchmark()
            }
        }
    }
    SemanticIndexer { id: backgroundIndex }
    Loader {
        id: view
        anchors.fill: parent
        active: false
        sourceComponent: SemanticPanel { indexer: backgroundIndex }
    }
}
