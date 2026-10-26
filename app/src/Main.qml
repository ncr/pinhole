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

    // The first run: no source yet, so where the photos are is all there is
    // to ask.
    readonly property bool firstRun: sources.known && sources.list.length === 0
    // Where the photos are, in a sentence: iCloud, or the folder's name
    // (an Apple ID's source is named after its owner, which reads wrong
    // there).
    readonly property string placeName: storage.sourceKind === "icloud" ? "iCloud" : storage.sourceName
    SourcePicker {
        id: firstRunPicker
        objectName: "firstRun"
        firstRun: true
        shown: win.firstRun
        onSignInRequested: icloudpd => signIn.open("")
    }

    // A source with nothing in it yet, and nothing under way to say so.
    Column {
        anchors.centerIn: parent
        visible: !win.firstRun && sources.known && grid.count === 0 && !buildProgress.visible && !trouble.visible
        spacing: theme.fontSize
        width: Math.min(parent.width - 48, 520)
        UiText {
            width: parent.width
            text: qsTr("Nothing in %1 yet").arg(win.placeName)
            color: theme.brightForeground
            font.pixelSize: Math.round(theme.fontSize * 1.333)
            horizontalAlignment: Text.AlignHCenter
        }
        UiText {
            width: parent.width
            text: qsTr("Photos show here as soon as they are found.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Where the photos come from, at the top left: the sources bar on a
    // click or Tab.
    ShellButton {
        id: sourcesButton
        objectName: "sourcesButton"
        floating: true
        visible: win.inGrid && !win.firstRun && sources.known
        x: 16
        y: 16
        // A cloud or a folder, from the Nerd Font Omarchy's type is.
        text: storage.sourceKind === "folder" ? "" : ""
        hint: "Tab"
        onTapped: bar.shown = true
    }

    // The date beside it: the year, month, days or date of what is on
    // screen, by zoom level (the grid's dateLabel), morphing as it changes.
    // For a moment after another source is shown, its name instead.
    property string switchedTo
    Timer { id: switchedTimer; interval: 2000; onTriggered: win.switchedTo = "" }
    Connections {
        target: sources
        function onSwitched(name) {
            win.switchedTo = name
            switchedTimer.restart()
        }
    }
    Chip {
        id: dateLabel
        objectName: "dateLabel"
        visible: win.inGrid && text !== "" && (grid.count > 0 || win.switchedTo !== "")
        x: sourcesButton.visible ? sourcesButton.x + sourcesButton.width + 6 : 16
        y: 16
        text: win.switchedTo !== "" ? win.switchedTo : grid.dateLabel
        morphing: true
        centred: false
        textColor: theme.brightForeground
        border.color: win.switchedTo !== "" ? theme.accent
                    : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
    }

    // A library being built: how far, at the foot.
    BuildProgress {
        id: buildProgress
        objectName: "buildProgress"
        name: win.placeName
        visible: win.inGrid && !win.firstRun && (phase === "index" || phase === "thumbs")
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
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

    // When the source cannot be reached: what still works, and what to do.
    // The photos on this computer always show.
    Chip {
        id: healthNotice
        readonly property string message: {
            switch (serviceHealth) {
            case "needs_sign_in": return qsTr("Not signed in to %1. Click to sign in again.").arg(storage.sourceAccount || win.placeName)
            // Out of reach: the network for a source that needs signing in,
            // the disk for a folder.
            case "offline": return storage.sourceNeedsSignIn ? qsTr("Offline. Showing what is on this computer.")
                                                             : qsTr("%1 is not there. Is its disk connected?").arg(storage.sourceName)
            case "failing": return qsTr("%1 is not answering properly. Trying again shortly.").arg(storage.sourceName)
            case "no_service": return qsTr("The Pinhole service is not running, so full-size photos and videos will not load.")
            default: return ""
            }
        }
        // Not while the middle says it.
        visible: message !== "" && grid.count > 0 && !trouble.visible
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
        // Signing in again, from here too.
        HoverHandler { enabled: serviceHealth === "needs_sign_in"; cursorShape: Qt.PointingHandCursor }
        TapHandler { enabled: serviceHealth === "needs_sign_in"; onTapped: signIn.open(storage.sourceAccount) }
    }

    // The Apple ID shown is not signed in to (Apple wants it again, or it was
    // signed out of): said once, in the middle, with the way to do it.
    // Looking around first leaves it to the corner.
    property bool troubleSeen: false
    // An Apple ID signed out of here, on purpose: the corner says so, the
    // middle need not.
    property string signedOutOf
    Connections {
        target: sources
        function onSignedOut(id) { win.signedOutOf = id }
    }
    // Another source: anything wrong with it is said afresh.
    Connections {
        target: storage
        function onSourceChanged() { win.troubleSeen = false }
    }
    readonly property string health: serviceHealth
    onHealthChanged: if (health !== "needs_sign_in") troubleSeen = false
    Rectangle {
        id: trouble
        objectName: "trouble"
        visible: win.inGrid && !win.firstRun && !win.troubleSeen && sources.current !== win.signedOutOf
                 && serviceHealth === "needs_sign_in" && storage.sourceKind === "icloud"
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, theme.fontSize * 37)
        height: troubleContent.implicitHeight + 2 * Math.round(theme.fontSize * 1.714)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.width: 1
        border.color: theme.red
        Column {
            id: troubleContent
            anchors.fill: parent
            anchors.margins: Math.round(theme.fontSize * 1.714)
            spacing: theme.fontSize
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Not signed in to %1").arg(storage.sourceAccount)
                color: theme.brightForeground
                font.pixelSize: Math.round(theme.fontSize * 1.143)
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Until you sign in again, you see the photos already on this computer, and nothing new arrives.")
            }
            Row {
                spacing: 8
                ShellButton {
                    active: true
                    text: qsTr("Sign in")
                    hint: "⏎"
                    onTapped: signIn.open(storage.sourceAccount)
                }
                ShellButton {
                    text: qsTr("Look around first")
                    hint: "Esc"
                    onTapped: win.troubleSeen = true
                }
            }
        }
    }
    Shortcut { enabled: trouble.visible; sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; onActivated: signIn.open(storage.sourceAccount) }
    Shortcut { enabled: trouble.visible; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: win.troubleSeen = true }

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
        visible: !photoViewer.shown && !win.panelUp
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

    SourcesBar {
        id: bar
        objectName: "sourcesBar"
        health: serviceHealth
        onAddRequested: {
            bar.shown = false
            addPicker.shown = true
        }
        onSignInRequested: appleId => signIn.open(appleId)
    }
    SourcePicker {
        id: addPicker
        objectName: "addSource"
        onSignInRequested: icloudpd => {
            addPicker.shown = false
            signIn.open("")
        }
    }
    SignInPanel {
        id: signIn
        objectName: "signIn"
        function open(appleId) {
            signIn.appleId = appleId
            signIn.shown = true
        }
    }
    // A folder added, or an Apple ID signed in to: what was open for it goes.
    Connections {
        target: sources
        function onSwitched() {
            addPicker.shown = false
            bar.shown = false
        }
    }
    HelpOverlay { id: help; objectName: "help" }
    SettingsPanel { id: settings; objectName: "settings" }

    // Something over the window that takes the keys.
    readonly property bool panelUp: help.shown || settings.shown || bar.shown || addPicker.shown || signIn.shown
    readonly property bool inGrid: !photoViewer.shown && !panelUp && !firstRun
    // The buttons' own keys (ShellButton.qml), but for while a panel is up.
    readonly property bool buttonKeys: !panelUp && !firstRun

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
    Shortcut { enabled: !settings.shown && !signIn.shown; sequence: "?"; context: Qt.ApplicationShortcut; onActivated: help.shown = !help.shown }
    Shortcut { enabled: help.shown; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: help.shown = false }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; onActivated: grid.statsVisible = !grid.statsVisible }
    Shortcut { enabled: !photoViewer.shown && !win.panelUp; sequence: ","; context: Qt.ApplicationShortcut; onActivated: settings.shown = true }

    // The sources: the bar on Tab, and any of the first nine shown on its
    // number, from the grid or the bar.
    Shortcut {
        enabled: (win.inGrid || bar.shown) && sources.known; sequence: "Tab"; context: Qt.ApplicationShortcut
        onActivated: bar.shown = !bar.shown
    }
    Repeater {
        model: 9
        delegate: Item {
            id: digit
            required property int index
            Shortcut {
                enabled: (win.inGrid || bar.shown) && digit.index < sources.list.length
                sequence: String(digit.index + 1)
                context: Qt.ApplicationShortcut
                onActivated: sources.showAt(digit.index)
            }
        }
    }

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
    Shortcut { enabled: win.inGrid && !trouble.visible; sequences: ["Space", "Return"]; context: Qt.ApplicationShortcut; onActivated: win.openCurrent() }

    Shortcut { enabled: win.inGrid && grid.selecting && !trouble.visible; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: grid.selecting = false }

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
