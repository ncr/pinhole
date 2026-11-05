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
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: backgroundIndex.summary
            }
            UiText {
                width: parent.width
                visible: text !== ""
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: backgroundIndex.profile?.name || ""
            }
            Item {
                width: parent.width
                height: indexing.implicitHeight
                UiText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Index new photos")
                }
                Toggle {
                    id: indexing
                    anchors.right: parent.right
                    Accessible.name: qsTr("Index new photos")
                    usable: backgroundIndex.catalogReady && !backgroundIndex.indexingJob
                    checked: !backgroundIndex.paused
                    onToggled: backgroundIndex.toggleIndexing()
                }
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
