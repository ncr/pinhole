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

    // The date at the top left: the year, month, days or date of what is on
    // screen, by zoom level (the grid's dateLabel), morphing as it changes.
    Chip {
        id: dateLabel
        objectName: "dateLabel"
        visible: win.inGrid && grid.count > 0 && text !== ""
        x: 16
        y: 16
        text: grid.dateLabel
        morphing: true
        centred: false
        textColor: theme.brightForeground
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
        // Beside the Newest button when it shows.
        anchors.left: newestButton.opacity > 0 ? newestButton.right : parent.left
        anchors.leftMargin: newestButton.opacity > 0 ? 8 : 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        // The sign-in and service problems in the theme's red; offline in
        // its muted text.
        text: (serviceHealth === "offline" ? "" : "● ") + message
        textColor: serviceHealth === "offline" ? theme.darkForeground : theme.red
        width: Math.min(parent.width - 32, label.implicitWidth + 2 * theme.fontSize)
        label.width: width - 2 * theme.fontSize
        label.elide: Text.ElideRight
    }

    // Back to the newest photos, whenever the grid is away from them.
    ShellButton {
        id: newestButton
        objectName: "newestButton"
        floating: true
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16
        // Above select mode's bar when the window is too narrow for both.
        anchors.bottomMargin: selectBar.visible && selectBar.x < 16 + width + 8 ? 20 + selectBar.height + 8 : 16
        readonly property bool wanted: win.inGrid && grid.count > 0 && !grid.atNewest
        opacity: wanted ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        text: "↓ " + qsTr("Newest")
        key: "N"
        onTapped: grid.glideToNewest()
    }

    // Every key, on ? or this.
    ShellButton {
        id: helpButton
        objectName: "helpButton"
        floating: true
        visible: win.inGrid && grid.count > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        // Above select mode's bar when the window is too narrow for both.
        anchors.bottomMargin: selectBar.visible && selectBar.x + selectBar.width > win.width - win.cornerWidth
                              ? 20 + selectBar.height + 8 : 16
        text: "?"
        width: height
        // The window's key: it also puts the help away.
        hint: ""
        onTapped: help.shown = true
    }

    // The settings, beside it: where photos are kept and how much.
    ShellButton {
        id: settingsButton
        objectName: "settingsButton"
        floating: true
        visible: helpButton.visible
        anchors.right: helpButton.left
        anchors.rightMargin: 6
        anchors.bottom: helpButton.bottom
        // A cog, from the Nerd Font Omarchy's type is.
        text: "\uf013"
        width: height
        onTapped: settings.shown = true
    }
    // What the two corner buttons take of the bottom right.
    readonly property real cornerWidth: 16 + helpButton.width + 6 + settingsButton.width + 8

    // Select mode, as in Photos on an iPhone: a button to enter it, and
    // while in it a bar with what is selected and what to do with it.
    ShellButton {
        id: selectButton
        objectName: "selectButton"
        floating: true
        visible: win.inGrid && grid.count > 0
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 16
        text: grid.selecting ? qsTr("Done") : qsTr("Select")
        key: "S"
        active: grid.selecting
        onTapped: grid.selecting = !grid.selecting
    }

    // The bar of select mode, as an Omarchy menu card: the theme's
    // background with a hairline in the text colour, the count in the
    // accent, rows lighting up under the pointer, and their keys beside them.
    // Few words, and fewer when the window is narrow (`fit`).
    Rectangle {
        id: selectBar
        objectName: "selectBar"
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

        // How much it says, from 0, everything, to 4: "Export originals"
        // becomes "Originals", then the keys go, then the count is only the
        // number, then not there (the ticks show it). The first that fits.
        readonly property int fit: {
            // Clear of the corner buttons, on both sides as it is centred.
            const room = win.width - 2 * win.cornerWidth
            for (let level = 0; level < 4; ++level)
                if (widthAt(level) <= room)
                    return level
            return 4
        }
        readonly property bool hints: fit < 2
        function countText(level) {
            const n = grid.selectionCount
            if (level >= 4)
                return ""
            if (level === 3)
                return n === 0 ? "" : String(n)
            return n === 0 ? qsTr("Select photos") : qsTr("%1 selected").arg(n)
        }
        function originalsText(level) { return level === 0 ? qsTr("Export originals") : qsTr("Originals") }
        // What the row is wide at a level, from its words: the count's
        // padding, then each button's (see ShellButton.qml).
        function widthAt(level) {
            const pad = 2 * Math.round(theme.fontSize * 0.833)
            const key = name => level < 2 ? barMetrics.advanceWidth("  " + name) : 0
            const count = countText(level)
            return 2 + (count === "" ? 0 : barMetrics.advanceWidth(count) + 2 * theme.fontSize)
                + Math.max(barMetrics.advanceWidth(qsTr("Copy")) + key("C"), barMetrics.advanceWidth("✓ " + qsTr("Copied"))) + pad
                + barMetrics.advanceWidth(qsTr("Export")) + key("E") + pad
                + barMetrics.advanceWidth(originalsText(level)) + key("⇧ E") + pad
        }
        FontMetrics { id: barMetrics; font.family: theme.font; font.pixelSize: theme.fontSize }

        Row {
            id: barRow
            anchors.centerIn: parent
            UiText {
                anchors.verticalCenter: parent.verticalCenter
                visible: text !== ""
                leftPadding: theme.fontSize
                rightPadding: theme.fontSize
                text: selectBar.countText(selectBar.fit)
                color: theme.accent
            }
            CopyButton {
                anchors.verticalCenter: parent.verticalCenter
                photos: () => grid.selection()
                enabledHere: grid.selectionCount > 0
                flat: true
                key: "C"
                hint: selectBar.hints ? "C" : ""
            }
            // As JPEG and MP4, what opens anywhere; the originals on Shift.
            ShellButton {
                anchors.verticalCenter: parent.verticalCenter
                flat: true
                usable: grid.selectionCount > 0
                text: qsTr("Export")
                key: "E"
                hint: selectBar.hints ? "E" : ""
                onTapped: outgoing.exportTo(grid.selection(), true)
            }
            ShellButton {
                anchors.verticalCenter: parent.verticalCenter
                flat: true
                usable: grid.selectionCount > 0
                text: selectBar.originalsText(selectBar.fit)
                key: "Shift+E"
                hint: selectBar.hints ? "⇧ E" : ""
                onTapped: outgoing.exportTo(grid.selection(), false)
            }
        }
    }

    HelpOverlay { id: help; objectName: "help" }
    SettingsPanel { id: settings; objectName: "settings" }

    readonly property bool inGrid: !photoViewer.shown && !help.shown && !settings.shown
    // The buttons' own keys (ShellButton.qml), but for while a panel is up.
    readonly property bool buttonKeys: !help.shown && !settings.shown

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
    Shortcut { enabled: !settings.shown; sequence: "?"; context: Qt.ApplicationShortcut; onActivated: help.shown = !help.shown }
    Shortcut { enabled: help.shown; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: help.shown = false }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; onActivated: grid.statsVisible = !grid.statsVisible }
    Shortcut { enabled: win.inGrid; sequence: ","; context: Qt.ApplicationShortcut; onActivated: settings.shown = true }

    // Every button presses on its own key too, shown beside it, while it
    // shows: Newest N, Select S, Copy C, Export (as JPEG and MP4) E,
    // originals ⇧E, Live L, close Esc. The keys here are the rest.

    // The grid.
    Shortcut { enabled: win.inGrid; sequences: ["+", "="]; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(-1) }
    Shortcut { enabled: win.inGrid; sequence: "-"; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(1) }
    Shortcut { enabled: win.inGrid; sequence: "Home"; context: Qt.ApplicationShortcut; onActivated: grid.scrollToStart() }
    // Up and down a row, held a steady glide; with Shift a half screen,
    // held faster.
    Shortcut { enabled: win.inGrid; sequence: "Up"; context: Qt.ApplicationShortcut; onActivated: grid.scrollByKey(-1, false) }
    Shortcut { enabled: win.inGrid; sequence: "Down"; context: Qt.ApplicationShortcut; onActivated: grid.scrollByKey(1, false) }
    Shortcut { enabled: win.inGrid; sequence: "Shift+Up"; context: Qt.ApplicationShortcut; onActivated: grid.scrollByKey(-1, true) }
    Shortcut { enabled: win.inGrid; sequence: "Shift+Down"; context: Qt.ApplicationShortcut; onActivated: grid.scrollByKey(1, true) }
    Shortcut { enabled: win.inGrid; sequence: "PgUp"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(-1) }
    Shortcut { enabled: win.inGrid; sequence: "PgDown"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(1) }
    Shortcut { enabled: win.inGrid; sequences: ["Space", "Return"]; context: Qt.ApplicationShortcut; onActivated: win.openCurrent() }

    Shortcut { enabled: win.inGrid && grid.selecting; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: grid.selecting = false }

    // Out of the app, from the grid or the viewer.
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; onActivated: outgoing.copy(win.chosen()) }
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; onActivated: outgoing.exportTo(win.chosen(), true) }
    Shortcut { sequence: "Ctrl+Shift+E"; context: Qt.ApplicationShortcut; onActivated: outgoing.exportTo(win.chosen(), false) }

    // The viewer.
    Shortcut {
        enabled: photoViewer.shown && !help.shown; sequences: ["Space", "Return"]; context: Qt.ApplicationShortcut
        // Space plays and pauses a video; on a photo it closes, like Quick Look.
        onActivated: if (!photoViewer.togglePlay()) photoViewer.closeViewer()
    }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Right"; context: Qt.ApplicationShortcut; onActivated: viewer.step(1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Left"; context: Qt.ApplicationShortcut; onActivated: viewer.step(-1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Shift+Left"; context: Qt.ApplicationShortcut; onActivated: photoViewer.skip(-10) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "Shift+Right"; context: Qt.ApplicationShortcut; onActivated: photoViewer.skip(10) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "M"; context: Qt.ApplicationShortcut; onActivated: photoViewer.muted = !photoViewer.muted }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequences: ["+", "="]; context: Qt.ApplicationShortcut; onActivated: viewer.zoomStep(1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "-"; context: Qt.ApplicationShortcut; onActivated: viewer.zoomStep(-1) }
    Shortcut { enabled: photoViewer.shown && !help.shown; sequence: "0"; context: Qt.ApplicationShortcut; onActivated: viewer.resetZoom() }
}
