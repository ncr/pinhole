import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import Pinhole

ApplicationWindow {
    id: win
    width: 1400
    height: 900
    visible: true
    title: "Pinhole"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    PhotoGrid {
        id: grid
        objectName: "grid"
        anchors.fill: parent
        focus: true
        accent: theme.accent
        onActivated: (index, rect) => viewer.openAt(index, rect)
        onMenuRequested: (index, pos) => photoMenu.popup(grid, pos)
    }

    // Every vsync while anything moves; the benchmark drives it too.
    FrameAnimation {
        running: grid.animating || (bench !== null && bench.running)
        onTriggered: {
            if (bench !== null)
                bench.tick(frameTime)
            grid.advance(frameTime)
        }
    }

    // First run: no library yet.
    Column {
        anchors.centerIn: parent
        visible: grid.count === 0
        spacing: 12
        width: Math.min(parent.width - 48, 520)
        Text {
            width: parent.width
            text: qsTr("No photo library yet")
            color: "white"
            font.pixelSize: 20
            horizontalAlignment: Text.AlignHCenter
        }
        Text {
            width: parent.width
            text: qsTr("Sign in to iCloud and build it in a terminal. It takes about ten minutes for 50,000 photos, and Pinhole opens it as soon as it is there.")
            color: "#c0ffffff"
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: commands.implicitWidth + 32
            height: commands.implicitHeight + 16
            radius: Math.min(6, theme.radius)
            color: "#202024"
            Text {
                id: commands
                anchors.centerIn: parent
                text: "pinhole-sync sign-in\npinhole-sync all"
                color: theme.accent
                font.family: "monospace"
                font.pixelSize: 14
                lineHeight: 1.4
            }
        }
        Text {
            width: parent.width
            visible: libraryProblem !== ""
            text: libraryProblem
            color: "#80ffffff"
            font.pixelSize: 11
            wrapMode: Text.WrapAnywhere
            horizontalAlignment: Text.AlignHCenter
        }
    }

    DateScrubber {
        grid: grid
        visible: win.inGrid
    }

    PhotoViewer {
        id: photoViewer
        anchors.fill: parent
        grid: grid
        onMenuRequested: (x, y) => photoMenu.popup(photoViewer, x, y)
    }

    // The right-click menu, in the grid and in the viewer: everything the
    // keys do, for a mouse.
    Menu {
        id: photoMenu
        objectName: "photoMenu"
        width: 280
        // Drawn here rather than by the style, whose shadow some renderers
        // skip: the same panel as the shortcuts.
        background: Rectangle {
            implicitWidth: 280
            radius: Math.min(8, theme.radius)
            color: "#f0161618"
            border.color: theme.accent
            border.width: 1
        }
        component Entry: MenuItem {
            height: visible ? implicitHeight : 0
        }
        Entry {
            visible: !viewer.open && grid.selectionCount === 1
            text: qsTr("Open")
            onTriggered: { const i = grid.selection()[0]; viewer.openAt(i, grid.rectOf(i)) }
        }
        Entry {
            visible: viewer.open && viewer.live
            text: qsTr("Play the Live Photo")
            onTriggered: viewer.playLive()
        }
        Entry {
            visible: viewer.open || grid.selectionCount > 0
            text: qsTr("Copy")
            onTriggered: outgoing.copy(win.chosen())
        }
        Entry {
            visible: viewer.open || grid.selectionCount > 0
            text: qsTr("Export originals")
            onTriggered: outgoing.exportTo(win.chosen(), false)
        }
        Entry {
            visible: viewer.open || grid.selectionCount > 0
            text: qsTr("Export as JPEG and MP4")
            onTriggered: outgoing.exportTo(win.chosen(), true)
        }
        Entry {
            visible: !viewer.open
            text: qsTr("Select all")
            onTriggered: grid.selectAll()
        }
        Entry {
            visible: viewer.open
            text: qsTr("Back to the library")
            onTriggered: photoViewer.closeViewer()
        }
    }

    Text {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 12
        visible: grid.statsVisible
        text: grid.statsText
        color: "white"
        font.family: "monospace"
        font.pixelSize: 12
        style: Text.Outline
        styleColor: "black"
        horizontalAlignment: Text.AlignRight
    }

    // What is on its way out: "Preparing 3 photos…", "Exported 12 photos".
    Rectangle {
        visible: outgoing.status !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: statusText.implicitWidth + 24
        height: statusText.implicitHeight + 12
        radius: Math.min(6, theme.radius)
        color: "#e0202024"
        Text {
            id: statusText
            anchors.centerIn: parent
            text: outgoing.status
            color: "white"
            font.pixelSize: 13
        }
    }

    // When iCloud cannot be reached: what still works, and what to do. The
    // photos on this computer always show.
    Rectangle {
        id: healthNotice
        readonly property string text: {
            switch (serviceHealth) {
            case "needs_sign_in": return qsTr("Not signed in to iCloud. To sign in again, run pinhole-sync sign-in in a terminal.")
            case "offline": return qsTr("Offline. Showing what is on this computer.")
            case "failing": return qsTr("iCloud is not answering properly. Trying again every minute.")
            case "no_service": return qsTr("The Pinhole service is not running, so full-size photos and videos will not load.")
            default: return ""
            }
        }
        visible: text !== "" && grid.count > 0
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16
        width: Math.min(parent.width - 32, healthText.implicitWidth + 40)
        height: healthText.implicitHeight + 14
        radius: Math.min(6, theme.radius)
        color: "#e0202024"
        Rectangle {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8
            radius: 4
            color: serviceHealth === "offline" ? "#9a9a9a" : "#e5a33d"
        }
        Text {
            id: healthText
            x: 28
            width: parent.width - 40
            anchors.verticalCenter: parent.verticalCenter
            text: healthNotice.text
            color: "white"
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    HelpOverlay { id: help; objectName: "help" }

    readonly property bool inGrid: !photoViewer.shown && !help.shown

    // What Copy and Export act on: the selection, else the photo in view.
    // What Copy and Export take: the photo on screen in the viewer, else the
    // selection, else the photo under the pointer.
    function chosen() {
        if (viewer.open)
            return [viewer.index]
        if (grid.selectionCount > 0)
            return grid.selection()
        return [grid.currentIndex()]
    }

    function openCurrent() {
        const index = grid.currentIndex()
        if (index >= 0)
            viewer.openAt(index, grid.rectOf(index))
    }

    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: help.shown = !help.shown }
    Shortcut { enabled: help.shown; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: help.shown = false }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; onActivated: grid.statsVisible = !grid.statsVisible }

    // The grid.
    Shortcut { enabled: win.inGrid; sequences: ["+", "="]; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(-1) }
    Shortcut { enabled: win.inGrid; sequence: "-"; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(1) }
    Shortcut { enabled: win.inGrid; sequence: "Home"; context: Qt.ApplicationShortcut; onActivated: grid.scrollToStart() }
    Shortcut { enabled: win.inGrid; sequence: "End"; context: Qt.ApplicationShortcut; onActivated: grid.scrollToEnd() }
    Shortcut { enabled: win.inGrid; sequence: "PgUp"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(-1) }
    Shortcut { enabled: win.inGrid; sequence: "PgDown"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(1) }
    Shortcut { enabled: win.inGrid; sequences: ["Space", "Return"]; context: Qt.ApplicationShortcut; onActivated: win.openCurrent() }

    Shortcut { enabled: win.inGrid; sequences: [StandardKey.SelectAll]; context: Qt.ApplicationShortcut; onActivated: grid.selectAll() }
    Shortcut { enabled: win.inGrid && grid.selectionCount > 0; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: grid.clearSelection() }

    // Out of the app, from the grid or the viewer.
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; onActivated: outgoing.copy(win.chosen()) }
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; onActivated: outgoing.exportTo(win.chosen(), false) }
    Shortcut { sequence: "Ctrl+Shift+E"; context: Qt.ApplicationShortcut; onActivated: outgoing.exportTo(win.chosen(), true) }

    // The viewer.
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: photoViewer.closeViewer() }
    Shortcut {
        enabled: photoViewer.shown && !help.shown; sequences: ["Space", "Return"]; context: Qt.ApplicationShortcut
        // Space plays and pauses a video; on a photo it closes, like Quick Look.
        onActivated: if (!photoViewer.togglePlay()) photoViewer.closeViewer()
    }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Right"; context: Qt.ApplicationShortcut; onActivated: viewer.step(1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Left"; context: Qt.ApplicationShortcut; onActivated: viewer.step(-1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "L"; context: Qt.ApplicationShortcut; onActivated: viewer.playLive() }
}
