import QtQuick
import Pinhole.Plugin

// Use added_date from the source, never the index's import time: rebuilding
// the catalog must not make the oldest photos appear newly received.
Item {
    id: recent
    objectName: "recentPhotos"
    // Its PluginView (Main.qml).
    property var view
    readonly property bool shown: view ? view.shown : false
    property bool loading: false
    property var rows: []
    property var pending: null
    property string problem: ""
    property string source: ""
    property int limit: 300
    property alias cursor: photos.cursor

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
            // Not those hidden from the grid (Screenshots'), as the grid.
            rows = found.filter(row => !pinhole.isHidden(row.id))
            const next = rows.findIndex(row => row.id === id)
            cursor = next >= 0 ? next : 0
        }).catch(error => {
            if (pending !== job)
                return
            pending = null
            loading = false
            if (error.code !== "cancelled" && error.code !== "reloaded")
                problem = qsTr("Could not read recent additions. Try opening this view again.")
        })
    }
    function start() {
        rows = []
        limit = 300
        cursor = 0
        refresh()
        photos.view.positionViewAtBeginning()
    }
    function stop() {
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
        // ← → through what it shows, newest addition first; back on the
        // one last looked at.
        pinhole.openPhotos(rows.slice(0, limit).map(r => r.id), row.id, { comeBack: true })
    }
    function reveal() {
        const row = rows[cursor]
        if (!row)
            return
        view.close()
        pinhole.reveal(row.id)
    }
    Connections {
        target: pinhole
        function onHiddenChanged() { if (recent.shown) recent.refresh() }
    }
    Connections {
        target: pinhole.library
        function onChanged() {
            if (recent.source !== pinhole.library.key)
                recent.rows = []
            if (recent.shown)
                recent.refresh()
        }
    }
    Connections {
        target: pinhole
        function onReturned(photo) {
            photos.moveTo(photo)
            recent.refresh()
        }
    }
    Component.onDestruction: { if (pending) pending.cancel() }

    Panel {}
    PanelHeader {
        id: titlebar
        title: qsTr("Recent additions")
    }
    UiText {
        id: heading
        anchors.top: titlebar.bottom
        x: 14
        width: parent.width - 28
        wrapMode: Text.WordWrap
        text: qsTr("Newest additions first. A received photo keeps the date it was taken in the main grid.")
        color: theme.darkForeground
    }
    UiText {
        anchors.centerIn: parent
        visible: loading && rows.length === 0 || problem !== "" || !loading && rows.length === 0
        text: problem || (loading ? qsTr("Reading the library…") : qsTr("No photos here yet."))
        color: theme.foreground
    }
    PhotoTiles {
        id: photos
        anchors.top: heading.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 16
        library: pinhole.library
        photos: recent.rows.slice(0, recent.limit).map(row => row.id)
        active: recent.shown && !pinhole.away
        minCell: theme.fontSize * 13
        captionHeight: theme.fontSize * 3.8
        caption: Column {
            property string photo
            property int index
            readonly property var row: recent.rows[index] ?? ({})
            UiText {
                width: parent.width
                elide: Text.ElideRight
                text: qsTr("Added %1").arg(new Date(parent.row.added_date || parent.row.asset_date).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                color: theme.brightForeground
            }
            UiText {
                width: parent.width
                elide: Text.ElideRight
                text: qsTr("Taken %1").arg(new Date(parent.row.asset_date).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                color: theme.foreground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
            }
        }
        onOpened: (photo, index) => recent.openPhoto(index)
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
        name: "recent.reveal"
        text: qsTr("Show the photo in the grid")
        section: qsTr("Recent additions")
        keys: ["Shift+Return", "Shift+Enter"]
        keyLayer: "plugin"
        when: recent.shown && recent.rows.length > 0
        onTriggered: recent.reveal()
    }
}
