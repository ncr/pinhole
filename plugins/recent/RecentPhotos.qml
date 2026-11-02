import QtQuick
import Pinhole.Plugin

// Use added_date from the source, never the index's import time: rebuilding
// the catalog must not make the oldest photos appear newly received.
Item {
    id: recent
    objectName: "recentPhotos"
    required property var plugin
    readonly property var pinhole: plugin.pinhole
    property bool shown: false
    property bool returning: false
    property bool loading: false
    property var rows: []
    property var pending: null
    property string problem: ""
    property string source: ""
    property int limit: 300
    property int cursor: 0
    visible: shown

    function refresh() {
        if (pending)
            pending.cancel()
        pending = null
        problem = ""
        source = pinhole.library.key
        if (pinhole.library.count === 0) {
            rows = []
            loading = false
            return
        }
        loading = true
        // One extra row tells whether there are more; ids survive a catalog
        // reload and photos stay individually accessible even inside stacks.
        const job = pinhole.query("SELECT id, asset_date, added_date FROM assets ORDER BY coalesce(added_date, asset_date) DESC, id DESC LIMIT ?", [limit + 1])
        pending = job
        job.then(found => {
            if (pending !== job)
                return
            pending = null
            loading = false
            const id = rows[cursor] ? rows[cursor].id : ""
            rows = found
            const next = rows.findIndex(row => row.id === id)
            cursor = next >= 0 ? next : 0
        }).catch(error => {
            if (pending !== job)
                return
            pending = null
            loading = false
            if (error.code !== "cancelled")
                problem = qsTr("Could not read recent additions. Try opening this view again.")
        })
    }
    function open() {
        rows = []
        limit = 300
        cursor = 0
        shown = true
        returning = false
        refresh()
        photos.positionViewAtBeginning()
    }
    function close() {
        shown = false
        returning = false
        if (pending)
            pending.cancel()
        pending = null
        loading = false
    }
    function openPhoto(at) {
        const row = rows[at]
        if (!row || pinhole.library.indexOf(row.id) < 0)
            return
        cursor = at
        returning = true
        shown = false
        pinhole.openPhoto(row.id)
    }
    function reveal() {
        const row = rows[cursor]
        if (!row)
            return
        close()
        pinhole.reveal(row.id)
    }
    function move(by) {
        cursor = Math.max(0, Math.min(Math.min(limit, rows.length) - 1, cursor + by))
        photos.positionViewAtIndex(cursor, GridView.Contain)
    }
    Connections {
        target: recent.pinhole.library
        function onChanged() {
            if (recent.source !== recent.pinhole.library.key) {
                recent.rows = []
                recent.returning = false
            }
            if (recent.shown)
                recent.refresh()
        }
    }
    Connections {
        target: recent.pinhole.viewer
        function onOpenChanged() {
            if (!recent.pinhole.viewer.open && recent.returning) {
                recent.shown = recent.pinhole.coveredBy === ""
                recent.returning = false
                if (recent.shown)
                    recent.refresh()
            }
        }
    }
    Connections {
        target: recent.pinhole
        function onDismissed() { recent.close() }
        function onCoveredByChanged() {
            if (recent.pinhole.coveredBy !== "" && recent.pinhole.coveredBy !== recent.pinhole.id)
                recent.returning = false
        }
    }
    Component.onDestruction: { if (pending) pending.cancel() }

    Panel {}
    Column {
        id: heading
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: dismiss.left
        anchors.margins: 16
        spacing: 6
        UiText {
            text: qsTr("Recent additions")
            color: theme.brightForeground
            font.pixelSize: Math.round(theme.fontSize * 1.5)
        }
        UiText {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Newest additions first. A received photo keeps the date it was taken in the main grid.")
            color: theme.foreground
        }
    }
    ShellButton {
        id: dismiss
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        text: "✕"
        action: closeCommand
    }
    UiText {
        anchors.centerIn: parent
        visible: loading && rows.length === 0 || problem !== "" || !loading && rows.length === 0
        text: problem || (loading ? qsTr("Reading the library…") : qsTr("No photos here yet."))
        color: theme.foreground
    }
    readonly property int columns: Math.max(1, Math.floor(photos.width / (theme.fontSize * 15)))
    ScrollGrid {
        id: photos
        anchors.top: heading.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 16
        model: Math.min(recent.limit, recent.rows.length)
        cellWidth: width / recent.columns
        cellHeight: cellWidth + theme.fontSize * 3.8
        delegate: Item {
            id: tile
            required property int index
            readonly property var row: recent.rows[index] ?? ({})
            readonly property var photo: (recent.pinhole.library.revision, recent.pinhole.library.item(row.id ?? ""))
            width: photos.cellWidth - 6
            height: photos.cellHeight - 6
            Rectangle {
                width: parent.width
                height: width
                color: theme.darkForeground
                Image {
                    anchors.fill: parent
                    source: tile.photo.thumbnail ?? ""
                    sourceSize: Qt.size(width * 1.5, height * 1.5)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: 2
                    border.color: theme.accent
                    visible: keymap.input === "keyboard" && recent.cursor === tile.index
                }
            }
            Column {
                y: parent.width + 5
                width: parent.width
                UiText {
                    width: parent.width
                    elide: Text.ElideRight
                    text: qsTr("Added %1").arg(new Date(tile.row.added_date || tile.row.asset_date).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                    color: theme.brightForeground
                }
                UiText {
                    width: parent.width
                    elide: Text.ElideRight
                    text: qsTr("Taken %1").arg(new Date(tile.row.asset_date).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                    color: theme.foreground
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                }
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: recent.openPhoto(tile.index) }
        }
    }
    Row {
        id: footer
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        spacing: 8
        ShellButton {
            text: qsTr("In the grid")
            enabled: recent.rows.length > 0
            onTapped: recent.reveal()
        }
        ShellButton {
            text: qsTr("Show more")
            visible: recent.rows.length > recent.limit
            enabled: !recent.loading
            onTapped: { recent.limit += 300; recent.refresh() }
        }
    }
    Command {
        id: closeCommand
        name: "recent.close"
        keys: ["Escape", "Shift+N"]
        keyLayer: "plugin"
        when: recent.shown
        onTriggered: recent.close()
    }
    Repeater {
        model: [["left", "Left", -1], ["right", "Right", 1], ["up", "Up", -recent.columns], ["down", "Down", recent.columns]]
        delegate: Command {
            required property var modelData
            name: "recent." + modelData[0]
            keys: [modelData[1]]
            keyLayer: "plugin"
            when: recent.shown
            autoRepeat: true
            onTriggered: recent.move(modelData[2])
        }
    }
    Command {
        name: "recent.open"
        text: qsTr("Open the photo")
        section: qsTr("Recent additions")
        keys: ["Return", "Enter"]
        keyLayer: "plugin"
        when: recent.shown && recent.rows.length > 0
        onTriggered: recent.openPhoto(recent.cursor)
    }
    Command {
        name: "recent.reveal"
        text: qsTr("Show the photo in the grid")
        section: qsTr("Recent additions")
        keys: ["Shift+Return", "Shift+Enter"]
        keyLayer: "plugin"
        when: recent.shown && recent.rows.length > 0
        onTriggered: recent.reveal()
    }
}
