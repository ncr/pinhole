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
    // Everything takes the Omarchy theme's colours (theme.*, colors.toml),
    // live: a light theme makes a light Pinhole.
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    PhotoGrid {
        id: grid
        objectName: "grid"
        anchors.fill: parent
        focus: true
        accent: theme.accent
        background: win.color
        onActivated: (index, rect) => viewer.openAt(index, rect)
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
        spacing: theme.fontSize
        width: Math.min(parent.width - 48, 520)
        UiText {
            width: parent.width
            text: qsTr("No photo library yet")
            color: theme.brightForeground
            font.pixelSize: Math.round(theme.fontSize * 1.333)
            horizontalAlignment: Text.AlignHCenter
        }
        UiText {
            width: parent.width
            text: qsTr("Sign in to iCloud and build it in a terminal. It takes about ten minutes for 50,000 photos, and Pinhole opens it as soon as it is there.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
        Chip {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "pinhole-sync sign-in\npinhole-sync all"
            textColor: theme.accent
            label.lineHeight: 1.4
        }
        UiText {
            width: parent.width
            visible: libraryProblem !== ""
            text: libraryProblem
            color: theme.darkForeground
            wrapMode: Text.WrapAnywhere
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Date headers at the top left, sticky as in HTML or Photos: each run of
    // photos from the same year, month or day (by zoom level, the grid's
    // dateHeaders) has one where it starts; the current one stays at the top
    // until the next comes up and pushes it out.
    Item {
        id: dateHeaders
        objectName: "dateHeaders"
        anchors.fill: parent
        visible: win.inGrid && grid.count > 0
        clip: true
        Repeater {
            model: 8
            delegate: Chip {
                required property int index
                readonly property var header: index < grid.dateHeaders.length ? grid.dateHeaders[index] : null
                readonly property var next: index + 1 < grid.dateHeaders.length ? grid.dateHeaders[index + 1] : null
                visible: header !== null
                x: 16
                // Where its run starts (the first one: at the top), and never
                // lower than where the next one would push it out from.
                y: {
                    if (!header)
                        return 0
                    const own = Math.max(16, header.top + 16)
                    return next ? Math.min(own, next.top + 16 - height - 8) : own
                }
                text: header ? header.label : ""
                textColor: theme.brightForeground
                label.font.pixelSize: Math.round(theme.fontSize * 1.167)
            }
        }
    }

    PhotoViewer {
        id: photoViewer
        objectName: "photoViewer"
        anchors.fill: parent
        grid: grid
    }

    Chip {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 12
        visible: grid.statsVisible
        text: grid.statsText
        label.horizontalAlignment: Text.AlignRight
    }

    // What is on its way out: "Preparing 3 photos…", "Exported 12 photos".
    Chip {
        visible: outgoing.status !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: grid.selecting ? 72 : 24
        text: outgoing.status
    }

    // When iCloud cannot be reached: what still works, and what to do. The
    // photos on this computer always show.
    Chip {
        id: healthNotice
        readonly property string message: {
            switch (serviceHealth) {
            case "needs_sign_in": return qsTr("Not signed in to iCloud. To sign in again, run pinhole-sync sign-in in a terminal.")
            case "offline": return qsTr("Offline. Showing what is on this computer.")
            case "failing": return qsTr("iCloud is not answering properly. Trying again every minute.")
            case "no_service": return qsTr("The Pinhole service is not running, so full-size photos and videos will not load.")
            default: return ""
            }
        }
        visible: message !== "" && grid.count > 0
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16
        // The sign-in and service problems in the theme's red; offline in
        // its muted text.
        text: (serviceHealth === "offline" ? "" : "● ") + message
        textColor: serviceHealth === "offline" ? theme.darkForeground : theme.red
        width: Math.min(parent.width - 32, label.implicitWidth + 2 * theme.fontSize)
        label.width: width - 2 * theme.fontSize
        label.elide: Text.ElideRight
    }

    // Select mode, as in Photos on an iPhone: a button to enter it, and
    // while in it a bar with what is selected and what to do with it.
    ShellButton {
        id: selectButton
        objectName: "selectButton"
        visible: win.inGrid && grid.count > 0
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 16
        text: grid.selecting ? qsTr("Done") : qsTr("Select")
        active: grid.selecting
        onTapped: grid.selecting = !grid.selecting
    }

    // The bar of select mode, as an Omarchy menu card: the theme's
    // background with a hairline in the text colour, the count in the
    // accent, rows lighting up under the pointer, and their keys beside them.
    Rectangle {
        id: selectBar
        visible: win.inGrid && grid.selecting
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: barRow.implicitWidth + 2
        height: barRow.implicitHeight + 2
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        Row {
            id: barRow
            anchors.centerIn: parent
            UiText {
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: theme.fontSize
                rightPadding: theme.fontSize
                text: grid.selectionCount === 0 ? qsTr("Click photos to select them")
                                                : qsTr("%n photo(s) selected", "", grid.selectionCount)
                color: theme.accent
            }
            CopyButton {
                anchors.verticalCenter: parent.verticalCenter
                photos: () => grid.selection()
                enabledHere: grid.selectionCount > 0
                flat: true
                hint: "ctrl c"
            }
            Repeater {
                model: [
                    { label: qsTr("Export originals"), keys: "ctrl e", act: () => outgoing.exportTo(grid.selection(), false) },
                    { label: qsTr("Export as JPEG and MP4"), keys: "ctrl ⇧ e", act: () => outgoing.exportTo(grid.selection(), true) },
                    { label: qsTr("Select all"), keys: "ctrl a", act: () => grid.selectAll() }
                ]
                delegate: ShellButton {
                    required property var modelData
                    required property int index
                    anchors.verticalCenter: parent.verticalCenter
                    flat: true
                    usable: grid.selectionCount > 0 || index === 2
                    text: modelData.label + "  <font color=\"" + theme.darkForeground + "\">" + modelData.keys + "</font>"
                    onTapped: modelData.act()
                }
            }
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
    Shortcut { enabled: win.inGrid && grid.selecting; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: grid.selecting = false }

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
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Shift+Left"; context: Qt.ApplicationShortcut; onActivated: photoViewer.skip(-10) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Shift+Right"; context: Qt.ApplicationShortcut; onActivated: photoViewer.skip(10) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "M"; context: Qt.ApplicationShortcut; onActivated: photoViewer.muted = !photoViewer.muted }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequences: ["+", "="]; context: Qt.ApplicationShortcut; onActivated: viewer.zoomStep(1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "-"; context: Qt.ApplicationShortcut; onActivated: viewer.zoomStep(-1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "0"; context: Qt.ApplicationShortcut; onActivated: viewer.resetZoom() }
}
