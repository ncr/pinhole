import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import Pinhole
import "keys.js" as KeyNames

ApplicationWindow {
    id: win
    width: 1400
    height: 900
    visible: true
    title: "Pinhole"
    // Everything takes the Omarchy theme's colours (theme.*, colors.toml),
    // live: a light theme makes a light Pinhole. With the stage under it
    // (stage.h), the window has an alpha channel and draws its own
    // background, which goes where the stage shows.
    color: stage.wanted ? "transparent" : theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    Rectangle {
        anchors.fill: parent
        visible: stage.wanted && !win.staged
        color: theme.background
    }

    PhotoGrid {
        id: grid
        objectName: "grid"
        anchors.fill: parent
        focus: true
        accent: theme.accent
        background: theme.background
        stacks: win.pluginStacks
        hidden: win.pluginHidden
        // Every kind of mark, Pinhole's and the plugins' (plugins.markKinds),
        // in the theme's colours; those switched on in the side panel drawn.
        markKinds: plugins.markKinds.map(k => ({ id: k.id, icon: k.icon, field: k.field, colour: win.markColour(k.colour) }))
        marksShown: (markChoices.revision, plugins.markKinds.filter(k => markChoices.shown(k.id, k.defaultShown)).map(k => k.id))
        // The keyboard's cursor, while the keyboard is what is in use.
        cursorShown: keymap.input === "keyboard"
        cursorColor: theme.brightForeground
        // The pointer is the grid's only while its keys are: under a panel,
        // a card, a menu, the viewer or a plugin's screen nothing that
        // comes through moves it, zooms it, selects or opens.
        covered: win.keyLayer !== "grid" || win.sourcesOff
        // No source shown, no photos: the library is library/none's, empty
        // (LibraryReloader), and until it is read the last one's are not
        // drawn under the card saying so. Not under the stage either.
        opacity: win.sourcesOff || win.staged ? 0 : 1
        onActivated: (index, rect) => viewer.openAt(index, rect)
    }
    // A mark's colour as a plugin names it (an action's too,
    // PluginGroup.qml): a theme colour's name, or "#rrggbb" as it is.
    function markColour(name) {
        switch (name) {
        case "red": return theme.red
        case "green": return theme.green
        case "yellow": return theme.yellow
        case "blue": return theme.blue
        case "magenta": return theme.magenta
        case "cyan": return theme.cyan
        case "accent": return theme.accent
        }
        return name
    }
    // The viewer's row passes over the photos the grid hides.
    Binding { target: viewer; property: "hidden"; value: win.pluginHidden }
    // A photo a plugin went to (pinhole.reveal): a ring around it that
    // fades, so it shows where it is even when it was on screen already.
    Rectangle {
        id: revealMark
        property int index: -1
        readonly property rect cell: (grid.scrollFraction, grid.columns, grid.count, index >= 0 ? grid.rectOf(index) : Qt.rect(0, 0, 0, 0))
        x: cell.x - 3
        y: cell.y - 3
        width: cell.width + 6
        height: cell.height + 6
        radius: 6
        color: "transparent"
        border.color: theme.accent
        border.width: 3
        opacity: 0
        visible: opacity > 0 && index >= 0 && !win.staged
        SequentialAnimation {
            id: revealFade
            NumberAnimation { target: revealMark; property: "opacity"; to: 1; duration: 120 }
            PauseAnimation { duration: 900 }
            NumberAnimation { target: revealMark; property: "opacity"; to: 0; duration: 600 }
        }
        Connections {
            target: plugins
            function onRevealed(index) {
                revealMark.index = index
                revealFade.restart()
            }
        }
    }
    // Back from the viewer, on the photo it closed on.
    Connections {
        target: viewer
        function onOpenChanged() {
            if (!viewer.open)
                grid.putCursorOn(viewer.index)
        }
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
    // to ask. Not over a library the window was given (--library): with no
    // source set up (a scratch HOME, pinhole-fake's library on a new
    // computer) the picker would cover it and take every key.
    readonly property bool firstRun: sources.known && sources.list.length === 0 && !libraryGiven && !sourcesOff
    // Where the photos come from is a plugin's (iCloud, Folders, one's
    // own): with those turned off, the window says which to turn on, with
    // the button, rather than offer a first run. Nothing of theirs is gone.
    // The plugins that bring sources, by id; Pinhole's own of them that are
    // off; and those off that the sources added are of.
    readonly property var sourcePlugins: (plugins.revision, plugins.ids.filter(id => (plugins.at(id).sources || []).length > 0))
    // Pinhole's own, in the service's order of kinds: iCloud's, then
    // folders'.
    readonly property var ownSourcePlugins: {
        const order = sources.kinds.map(k => k.plugin)
        const place = id => order.indexOf(id) < 0 ? order.length : order.indexOf(id)
        return (plugins.revision, sourcePlugins.filter(id => !plugins.at(id).personal)
                .map((id, i) => ({ id: id, i: i })).sort((a, b) => place(a.id) - place(b.id) || a.i - b.i).map(x => x.id))
    }
    readonly property var ownSourcePluginsOff: (plugins.revision, ownSourcePlugins.filter(id => !plugins.at(id).enabled))
    readonly property var addedPluginsOff: {
        const owners = []
        for (const kind of (sources.list, plugins.revision, sources.addedKinds())) {
            const told = sources.kinds.find(k => k.kind === kind)
            const owner = told ? told.plugin : sourcePlugins.find(id => plugins.at(id).sources.indexOf(kind) >= 0)
            if (owner && owners.indexOf(owner) < 0 && plugins.ids.indexOf(owner) >= 0 && !plugins.at(owner).enabled)
                owners.push(owner)
        }
        return owners
    }
    readonly property bool sourcesOff: sources.known && sources.list.length === 0 && !libraryGiven
                                       && (addedPluginsOff.length > 0 || !sourcePlugins.some(id => plugins.at(id).enabled))
    function pluginNames(ids) { return ids.map(id => plugins.at(id).name).join(", ") }
    // The source shown, as its plugin is given it (PluginSource.qml): what
    // the service says of it, its row's count, and how it is doing.
    readonly property var shownSource: {
        const entry = sources.list[sources.currentIndex]
        return Object.assign({}, storage.source, entry && entry.id === storage.source.id ? entry : {},
                             { shown: true, health: service.health })
    }
    // Its kind as its plugin shows it, null when no plugin on shows it.
    readonly property var shownKind: shownSource.kind ? win.pluginSources[shownSource.kind] ?? null : null
    // Where the photos are, in a sentence, as its plugin says: iCloud, or
    // the folder's name (an Apple ID's source is named after its owner,
    // which reads wrong there).
    readonly property string placeName: shownKind && shownKind.place ? shownKind.place(shownSource) : storage.sourceName
    // Signing in to a source, as its plugin does it: over the window, with
    // what was open for it put away.
    function canSignIn(source) {
        const kind = source && source.kind ? win.pluginSources[source.kind] : null
        return !!(kind && kind.signIn)
    }
    function signInTo(source) {
        if (!canSignIn(source))
            return
        side.shown = false
        showAddSource(false)
        win.pluginSources[source.kind].signIn(source)
    }
    Loader {
        anchors.fill: parent
        property bool opened: false
        active: win.firstRun || opened
        onLoaded: opened = true
        sourceComponent: SourcePicker {
            objectName: "firstRun"
            firstRun: true
            // Under a plugin's own screen (iCloud's sign-in).
            shown: win.firstRun
            kinds: win.pluginSourceList
        }
    }

    // The plugins sources come from all off: which to turn on, each a
    // button (1, 2), and the side panel for the rest. One's own copy is
    // not turned on from here: its button opens the side panel at its
    // switch, which says what trusting it means.
    readonly property var sourcesOffButtons: {
        const ids = addedPluginsOff.concat(ownSourcePluginsOff.filter(id => addedPluginsOff.indexOf(id) < 0))
        return (plugins.revision, ids.slice(0, 9))
    }
    readonly property var personalSourcesOff: (plugins.revision, addedPluginsOff.filter(id => plugins.at(id).personal))
    Rectangle {
        id: sourcesOffCard
        objectName: "sourcesOff"
        anchors.centerIn: parent
        visible: win.sourcesOff && win.chromeUp
        opacity: win.chrome
        width: Math.min(parent.width - 32, 600)
        height: sourcesOffContent.implicitHeight + 2 * Math.round(theme.fontSize * 1.714)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.width: 1
        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
        // Over whatever is beneath: a click on it reaches nothing there.
        Shield { anchors.fill: parent }
        Column {
            id: sourcesOffContent
            anchors.centerIn: parent
            spacing: theme.fontSize
            width: parent.width - 2 * Math.round(theme.fontSize * 1.714)
            UiText {
                width: parent.width
                text: win.addedPluginsOff.length > 0 ? qsTr("Your sources' plugins are off: %1").arg(win.pluginNames(win.addedPluginsOff))
                                                     : qsTr("No source of photos is on")
                color: theme.brightForeground
                font.pixelSize: Math.round(theme.fontSize * 1.333)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
            UiText {
                width: parent.width
                text: win.addedPluginsOff.length > 0
                      ? qsTr("Their sources and photos are still on this computer: turned on, they show again at once.")
                      : qsTr("Where the photos come from is a plugin's: %1.").arg(win.pluginNames(win.ownSourcePlugins))
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
            UiText {
                visible: win.personalSourcesOff.length > 0
                width: parent.width
                text: (win.personalSourcesOff.length === 1
                       ? qsTr("Your own copy of %1 is off: the side panel says what it can see, beside its switch.")
                       : qsTr("Your own copies of %1 are off: the side panel says what they can see, beside their switches."))
                      .arg(win.pluginNames(win.personalSourcesOff))
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
            Flow {
                // Centred, a line after another as the card has room.
                width: Math.min(parent.width, buttonsWidth)
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                topPadding: Math.round(theme.fontSize * 0.5)
                readonly property real buttonsWidth: {
                    let w = 0
                    for (let i = 0; i < turnOnButtons.count; ++i)
                        w += (turnOnButtons.itemAt(i)?.width ?? 0) + spacing
                    return Math.max(0, w - spacing)
                }
                Repeater {
                    id: turnOnButtons
                    // Those the sources added are of first.
                    model: win.sourcesOffButtons
                    delegate: ShellButton {
                        required property string modelData
                        required property int index
                        readonly property bool personal: plugins.at(modelData).personal === true
                        objectName: "turnOn:" + modelData
                        active: index === 0
                        text: (personal ? qsTr("Turn on %1…") : qsTr("Turn on %1")).arg(plugins.at(modelData).name)
                        key: String(index + 1)
                        keyLayer: "grid"
                        usable: win.sourcesOff
                        onTapped: {
                            if (!personal) {
                                plugins.setEnabled(modelData, true)
                                return
                            }
                            side.shown = true
                            side.showPlugin(modelData)
                        }
                    }
                }
            }
            UiText {
                width: parent.width
                text: qsTr("Or in the side panel (Tab), under Plugins.")
                color: theme.darkForeground
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    // A source with nothing in it yet, and nothing under way to say so.
    Column {
        anchors.centerIn: parent
        visible: !win.firstRun && !win.sourcesOff && sources.known && grid.count === 0 && grid.hiddenCount === 0 && !buildProgress.visible
                 && !trouble.visible
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

    // A source whose every photo a plugin hides (Screenshots, on a folder
    // of screenshots): it is not empty, and the way back is that plugin's.
    Column {
        anchors.centerIn: parent
        visible: !win.firstRun && !win.sourcesOff && sources.known && grid.count === 0 && grid.hiddenCount > 0 && !trouble.visible
        spacing: theme.fontSize
        width: Math.min(parent.width - 48, 520)
        UiText {
            width: parent.width
            text: qsTr("All %n photos here are hidden", "", grid.hiddenCount)
            color: theme.brightForeground
            font.pixelSize: Math.round(theme.fontSize * 1.333)
            horizontalAlignment: Text.AlignHCenter
        }
        UiText {
            width: parent.width
            text: qsTr("%1 hides them. Its button at the top, or its settings in the side panel (Tab), shows them again.")
                  .arg(win.pluginHiders.join(", "))
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
        visible: win.chromeUp && !win.firstRun && sources.known
        opacity: win.chrome
        x: 16
        y: 16
        // What it opens: a panel at the left (the Nerd Font's
        // layout-sidebar-left, outlined in the weight of the letters).
        text: "\uebf3"
        action: sidePanel
        hint: "Tab"
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
        Accessible.role: win.inGrid ? Accessible.Button : Accessible.StaticText
        Accessible.name: win.inGrid ? qsTr("Photo order") : text
        Accessible.description: text
        Accessible.expanded: orderMenu.open
        Accessible.onPressAction: if (win.inGrid) orderMenu.toggle(true)
        visible: win.chromeUp && !win.firstRun && text !== "" && (grid.count > 0 || win.switchedTo !== "")
        opacity: win.chrome
        x: sourcesButton.visible ? sourcesButton.x + sourcesButton.width + 6 : 16
        y: 16
        text: (win.switchedTo !== "" ? win.switchedTo : grid.dateLabel) + (win.inGrid ? "  ▾" : "")
        // A long date must not share its tap area with More or Select.
        // Folding still uses naturalWidth below, so this clamp cannot
        // alternate the row between unfolded and folded layouts.
        width: Math.max(0, Math.min(naturalWidth,
            (gridActions.visible && gridActions.width > 0 ? gridActions.x
             : selectButton.visible ? selectButton.x : win.width - 16) - x - 8))
        morphing: true
        centred: false
        textColor: theme.brightForeground
        readonly property bool hot: dateHover.hovered || orderMenu.open
        color: Qt.tint(Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92),
                       Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b,
                               dateTap.pressed ? 0.22 : hot ? 0.08 : 0))
        border.color: win.switchedTo !== "" ? theme.accent
                    : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, hot ? 0.25 : 0.4)
        Behavior on color { ColorAnimation { duration: 100 } }
        Shield { anchors.fill: parent; z: -1 }
        HoverHandler { id: dateHover; enabled: win.inGrid; cursorShape: Qt.PointingHandCursor }
        TapHandler { id: dateTap; enabled: win.inGrid; onTapped: orderMenu.toggle(false) }
    }
    PhotoOrderMenu {
        id: orderMenu
        objectName: "orderMenu"
        z: 1001
        button: dateLabel
        available: dateLabel.visible && win.inGrid
        value: timeline.mode
        onChosen: mode => timeline.mode = mode
    }

    // A library being built: how far, at the foot.
    BuildProgress {
        id: buildProgress
        objectName: "buildProgress"
        name: win.placeName
        visible: win.chromeUp && !win.firstRun && (phase === "index" || phase === "thumbs")
        opacity: win.chrome
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
    }

    // Made once Qt Multimedia, which plays its videos, is ready: that takes
    // most of a second, done on a thread of its own while the grid shows
    // (main.cpp). A photo opened before then opens as soon as it is.
    property bool mediaReady: false
    Loader {
        id: viewerLoader
        anchors.fill: parent
        active: win.mediaReady
        asynchronous: true
        // Inside the PhotoViewer, grid would be its own property.
        readonly property PhotoGrid photoGrid: grid
        sourceComponent: PhotoViewer {
            objectName: "photoViewer"
            grid: viewerLoader.photoGrid
            helpAction: helpToggle
            pluginActions: win.pluginActions.filter(a => a.place === "viewer")
            pluginLayers: win.pluginLayers
        }
    }
    readonly property PhotoViewer photoViewer: viewerLoader.item
    readonly property bool viewerShown: photoViewer?.shown ?? false

    // Wide gamut and HDR under the settled photo (Stage.qml): made only
    // where the display can show them, or forced (stage.h); a child window
    // of this one.
    readonly property var stageWindow: stage.window
    // The stage shows the photo: the window transparent there.
    readonly property bool staged: stage.window?.handedOver ?? false
    Binding { target: win.photoViewer; property: "staged"; value: win.staged; when: win.photoViewer !== null }
    readonly property bool pluginsHoldSdr: pluginPlane?.holdSdr ?? false

    Chip {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 12
        visible: grid.statsVisible
        text: grid.statsText
        label.horizontalAlignment: Text.AlignRight
        Shield { anchors.fill: parent; z: -1 }
    }

    // Files dropped on the window on their way in, above a library being
    // built if one is.
    AddProgress {
        id: addProgress
        objectName: "addProgress"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: buildProgress.visible ? buildProgress.top : parent.bottom
        anchors.bottomMargin: buildProgress.visible ? 8 : grid.selecting ? 72 : 24
    }
    Command {
        name: "add.dismiss"
        keys: ["Escape"]
        keyLayer: "grid"
        when: addProgress.visible && !incoming.busy && !grid.selecting && !trouble.visible
        mouse: qsTr("Click it")
        touchpad: qsTr("Tap it")
        onTriggered: incoming.dismiss()
    }

    // What is on its way out: "Preparing 3 photos…", "Exported 12 photos".
    Chip {
        objectName: "outgoingNotice"
        visible: outgoing.status !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: grid.selecting ? 72 : 24
        text: outgoing.status
        Shield { anchors.fill: parent; z: -1 }
    }

    // When the source cannot be reached: what still works, and what to do.
    // The photos on this computer always show.
    Chip {
        id: healthNotice
        readonly property string message: {
            switch (service.health) {
            case "needs_sign_in": return qsTr("Not signed in to %1. Click to sign in again.").arg(storage.sourceAccount || win.placeName)
            // Out of reach: the network for a source that needs signing in,
            // the disk for a folder.
            case "offline": return storage.sourceNeedsSignIn ? qsTr("Offline. Showing what is on this computer.")
                                                             : qsTr("%1 is not there. Is its disk connected?").arg(storage.sourceName)
            case "failing": return qsTr("%1 is not answering properly. Trying again shortly.").arg(storage.sourceName)
            case "no_service": return qsTr("The Pinhole service is not running, so full-size photos and videos will not load.")
            case "incompatible": return qsTr("The Pinhole service running is another version, so some things may not work.")
            default: return ""
            }
        }
        // Not while the middle says it.
        visible: message !== "" && grid.count > 0 && !trouble.visible
        // Beside the Newest button when it shows.
        anchors.left: newestButton.opacity > 0 ? newestButton.right : parent.left
        anchors.leftMargin: newestButton.opacity > 0 ? 8 : 16
        anchors.bottom: parent.bottom
        // Over the viewer, above its row of photos and a video's bar: a
        // tap meant for one of them never signs in.
        anchors.bottomMargin: win.viewerShown ? win.photoViewer.stripHeight + 24 : 16
        // The sign-in and service problems in the theme's red; offline in
        // its muted text.
        text: (service.health === "offline" ? "" : "● ") + message
        textColor: service.health === "offline" ? theme.darkForeground : theme.red
        width: Math.min(parent.width - 32, label.implicitWidth + 2 * theme.fontSize)
        label.width: width - 2 * theme.fontSize
        label.elide: Text.ElideRight
        // Signing in again, from here too, or with I.
        HoverHandler { enabled: signInAgain.when; cursorShape: Qt.PointingHandCursor }
        TapHandler { enabled: signInAgain.when; onTapped: signInAgain.trigger() }
        // Beneath its own tap, which sees a press first.
        Shield { anchors.fill: parent; z: -1 }
    }
    Command {
        id: signInAgain
        name: "health.signIn"
        text: qsTr("Sign in again")
        keys: ["I"]
        keyLayer: "grid"
        when: service.health === "needs_sign_in" && healthNotice.visible && win.canSignIn(win.shownSource)
        mouse: qsTr("Click the notice at the foot")
        touchpad: qsTr("Tap the notice at the foot")
        onTriggered: win.signInTo(win.shownSource)
    }

    // The source shown is not signed in to (Apple wants it again, or it was
    // signed out of): said once, in the middle, with the way to do it, its
    // plugin's (PluginSource.signIn). Looking around first leaves it to the
    // corner.
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
    readonly property string health: service.health
    onHealthChanged: if (health !== "needs_sign_in") troubleSeen = false
    Rectangle {
        id: trouble
        objectName: "trouble"
        visible: win.chromeUp && !win.firstRun && !win.troubleSeen && sources.current !== win.signedOutOf
                 && service.health === "needs_sign_in" && win.canSignIn(win.shownSource)
        opacity: win.chrome
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
                text: qsTr("Not signed in to %1").arg(storage.sourceAccount || storage.sourceName)
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
                    action: troubleSignIn
                }
                ShellButton {
                    text: qsTr("Look around first")
                    action: troubleLater
                }
            }
        }
    }
    Command {
        id: troubleSignIn
        name: "trouble.signIn"
        keys: ["Return", "Enter"]
        keysText: "⏎"
        keyLayer: "grid"
        when: trouble.visible
        onTriggered: win.signInTo(win.shownSource)
    }
    Command {
        id: troubleLater
        name: "trouble.later"
        keys: ["Escape"]
        keyLayer: "grid"
        when: trouble.visible
        onTriggered: win.troubleSeen = true
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
        anchors.bottomMargin: (selectBar?.visible ?? false) && selectBar.x < 16 + width + 8 ? 20 + selectBar.height + 8 : 16
        readonly property bool wanted: win.inGrid && grid.count > 0 && !grid.atNewest
        // Its own fade (it comes and goes with the scroll), and the grid's.
        property real shown: wanted ? 1 : 0
        Behavior on shown { NumberAnimation { duration: 150 } }
        opacity: shown * win.chrome
        visible: opacity > 0
        text: "↓ " + qsTr("Newest")
        action: newest
    }

    // A quiet invitation, only over the idle grid. Dismissing it keeps the
    // update available in settings; no photo or keyboard focus is taken.
    UpdateStatus { id: updateStatus }
    Row {
        id: updateNotice
        objectName: "updateNotice"
        property bool dismissed: false
        visible: win.inGrid && !grid.selecting && !win.firstRun && win.chromeUp
                 && !dismissed && ["available", "updating", "ready"].indexOf(updater.state) >= 0
        anchors.right: parent.right
        anchors.bottom: helpButton.top
        anchors.rightMargin: 16
        anchors.bottomMargin: 8
        spacing: 4
        ShellButton {
            floating: true
            text: updater.state === "updating" ? qsTr("Updating…")
                  : updater.state === "ready" ? qsTr("Restart to finish updating") : qsTr("Update available")
            usable: !updateStatus.busy
            busy: updater.state === "updating"
            onTapped: updateStatus.press()
        }
        ShellButton {
            floating: true
            text: "×"
            width: height
            onTapped: updateNotice.dismissed = true
        }
        Connections {
            target: updater
            function onChanged() { if (updater.state === "ready") updateNotice.dismissed = false }
        }
    }

    // Every key, on ? or this. Settings are in the side panel (Tab or ",").
    ShellButton {
        id: helpButton
        objectName: "helpButton"
        floating: true
        visible: win.chromeUp
        opacity: win.chrome
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        // Above select mode's bar when the window is too narrow for both.
        anchors.bottomMargin: (selectBar?.visible ?? false) && selectBar.x + selectBar.width > win.width - win.cornerWidth
                              ? 20 + selectBar.height + 8 : 16
        text: "?"
        width: height
        action: helpToggle
        // Its label is the key.
        hint: ""
    }

    // Closer and further, beside it: for a mouse with no side wheel to
    // turn, and a touchpad that does not pinch.
    Row {
        id: zoomButtons
        objectName: "zoomButtons"
        visible: win.chromeUp && !win.firstRun && grid.count > 0
        opacity: win.chrome
        anchors.right: helpButton.left
        anchors.rightMargin: 6
        anchors.bottom: helpButton.bottom
        spacing: 6
        ShellButton {
            floating: true
            text: "−"
            width: height
            hint: ""
            action: gridZoomOut
        }
        ShellButton {
            floating: true
            text: "+"
            width: height
            hint: ""
            action: gridZoomIn
        }
    }

    // What the corner buttons take of the bottom right.
    readonly property real cornerWidth: 16 + helpButton.width + 6 + zoomButtons.width + 8

    // Select mode, as in Photos on an iPhone: a button to enter it, and
    // while in it a bar with what is selected and what to do with it.
    ShellButton {
        id: selectButton
        objectName: "selectButton"
        floating: true
        visible: win.chromeUp && !win.firstRun && grid.count > 0
        opacity: win.chrome
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 16
        text: grid.selecting ? qsTr("Done") : qsTr("Select")
        action: selectToggle
        active: grid.selecting
    }
    Command {
        id: selectToggle
        name: "select.toggle"
        text: qsTr("Select, Done")
        section: "select"
        keys: ["S"]
        keyLayer: "grid"
        when: grid.count > 0
        onTriggered: grid.selecting = !grid.selecting
    }
    Command {
        name: "select.all"
        text: qsTr("Select all")
        section: "select"
        keys: ["Ctrl+A"]
        keyLayer: "grid"
        when: grid.count > 0 && !trouble.visible
        mouse: qsTr("Range")
        touchpad: qsTr("Range")
        onTriggered: grid.selectAll()
    }

    // Plugins' buttons for the grid, before Select (not in select mode,
    // which has its own), right of the date: their keys go when they do
    // not fit there, then they fold into More ▾ (`fit`).
    Row {
        id: gridActions
        objectName: "gridActions"
        visible: win.chromeUp && !win.firstRun && !win.sourcesOff && !grid.selecting
        opacity: win.chrome
        anchors.top: parent.top
        anchors.right: selectButton.visible ? selectButton.left : parent.right
        anchors.topMargin: 16
        anchors.rightMargin: selectButton.visible ? 6 : 16
        spacing: 6
        readonly property var actions: win.pluginActions.filter(a => a.place === "grid")
        // 0: as they are; 1: without their keys; 2: in More ▾. The first
        // that fits from the words (ShellButton.qml's measures), as select
        // mode's bar does.
        readonly property int fit: {
            // The date's width a step at a time, so that scrolling through
            // months of longer and shorter names does not refold them.
            const step = 3 * theme.fontSize
            const date = dateLabel.visible ? Math.ceil(dateLabel.naturalWidth / step) * step : 0
            const right = selectButton.visible ? win.width - selectButton.x + 6 : 16
            const room = win.width - right - (dateLabel.visible ? dateLabel.x + date + 8 : 16)
            for (let level = 0; level < 2; ++level)
                if (widthAt(level) <= room)
                    return level
            return 2
        }
        function widthAt(level) {
            const pad = 2 * Math.round(theme.fontSize * 0.833)
            const shown = actions.filter(a => a.shown)
            let width = 0
            for (const a of shown)
                width += gridMetrics.of(a.text) + pad + spacing
                    + (level < 1 && a.key !== "" ? gridMetrics.of(" " + KeyNames.nameOf(a.key)) : 0)
            return Math.max(0, width - spacing)
        }
        LabelMetrics { id: gridMetrics }
        Repeater {
            model: gridActions.actions
            delegate: ShellButton {
                id: gridAction
                required property var modelData
                floating: true
                visible: modelData.shown && gridActions.fit < 2
                usable: modelData.usable
                active: modelData.active
                labelColor: modelData.textColour !== "" ? win.markColour(modelData.textColour) : theme.foreground
                busy: modelData.busy
                busyDelay: modelData.busyDelay
                text: modelData.text
                key: modelData.key
                hint: gridActions.fit < 1 ? KeyNames.nameOf(modelData.key) : ""
                keyLayer: "grid"
                onTapped: modelData.triggered()
                onHoveredChanged: modelData.hovered = hovered
                // What it could not do, under it (PluginAction.complain).
                transform: Translate { id: gridNudge }
                Connections {
                    target: gridAction.modelData
                    function onComplained(text) {
                        if (!gridAction.visible)
                            return
                        gridComplaint.active = true
                        gridComplaint.item.say(text)
                    }
                }
                Loader {
                    id: gridComplaint
                    active: false
                    sourceComponent: Complaint { button: gridAction; nudged: gridNudge }
                }
                // It stands for its action, for a tour to light; back from
                // More, again.
                onVisibleChanged: if (visible) modelData.button = gridAction
                Component.onCompleted: modelData.button = gridAction
                Component.onDestruction: {
                    if (hovered)
                        modelData.hovered = false
                    if (modelData.button === gridAction)
                        modelData.button = null
                }
            }
        }
        MoreGroup {
            id: gridMore
            objectName: "gridMore"
            visible: gridActions.fit >= 2 && menuItems.length > 0
            floating: true
            groups: gridActions.actions.map(a => [a])
            keyLayer: "grid"
        }
    }

    // The bar of select mode, as the viewer's (ActionBar.qml): the count in
    // the accent, Range, Copy with Export in its menu, and the plugins'
    // groups.
    // Fewer words when the window is narrow (`fit`).
    property var selectBar: null
    // Reserve the original stacking position below plugin/modal overlays;
    // the controls themselves are made only on the first selection.
    Item { id: selectionControlsLayer; anchors.fill: parent }
    Connections {
        target: grid
        function onSelectingChanged() {
            if (grid.selecting && win.selectBar === null)
                win.selectBar = selectionBarComponent.createObject(win, { parent: selectionControlsLayer })
        }
    }
    Component {
        id: selectionBarComponent
        ActionBar {
            id: selectBar
            objectName: "selectBar"
            visible: win.chromeUp && !win.firstRun && grid.selecting
            opacity: win.chrome
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20

            // How much it says, from 0, everything, to 3: the keys go, then the
            // count is only the number, then not there (the ticks show it). The
            // first that fits.
            readonly property int fit: {
                // Clear of the corner buttons, on both sides as it is centred.
                const room = win.width - 2 * win.cornerWidth
                for (let level = 0; level < 3; ++level)
                    if (widthAt(level) <= room)
                        return level
                return 3
            }
            readonly property bool hints: fit < 1
            function countText(level) {
                const n = grid.selectionCount
                if (level >= 3)
                    return ""
                if (level === 2)
                    return n === 0 ? "" : String(n)
                return n === 0 ? qsTr("Select photos") : qsTr("%1 selected").arg(n)
            }
            // Plugins' groups for what is ticked: those of `order` below 0
            // before Copy, the rest after it.
            readonly property var pluginGroups: groupsOf(win.pluginActions.filter(a => a.place === "selection"))
            readonly property var groupsBefore: pluginGroups.filter(group => group[0].order < 0)
            readonly property var groupsAfter: pluginGroups.filter(group => group[0].order >= 0)
            // What the row is wide at a level, from its words: the count's
            // padding, then each group's button, ▾ and hairline (see
            // ShellButton.qml and MenuGroup.qml).
            function widthAt(level) {
                const pad = 2 * Math.round(theme.fontSize * 0.833)
                const arrow = barMetrics.of("▾") + 2 * Math.round(theme.fontSize * 0.5)
                const key = name => level < 1 ? barMetrics.of("  " + name) : 0
                const count = countText(level)
                let width = 2 + (count === "" ? 0 : barMetrics.of(count) + 2 * theme.fontSize + 5)
                    + barMetrics.of(qsTr("Range")) + key("R") + pad + 5
                    + Math.max(barMetrics.of(qsTr("Copy")) + key("C"), barMetrics.of("✓ " + qsTr("Copied"))) + pad + arrow
                for (const group of pluginGroups) {
                    const shown = group.filter(a => a.shown)
                    if (shown.length > 0)
                        width += 5 + barMetrics.of(shown[0].text) + key(KeyNames.nameOf(shown[0].key)) + pad
                            + (shown.length > 1 ? arrow : 0)
                }
                return width
            }
            LabelMetrics { id: barMetrics }

            UiText {
                anchors.verticalCenter: parent.verticalCenter
                visible: text !== ""
                leftPadding: theme.fontSize
                rightPadding: theme.fontSize
                text: selectBar.countText(selectBar.fit)
                color: theme.accent
            }
            // Range: the next photo tapped ticks all from the last ticked to it.
            MenuGroup {
                objectName: "selectRangeGroup"
                menuText: qsTr("Range")
                menuKey: "Shift+R"
                keyLayer: "grid"
                upward: true
                items: [{ text: qsTr("Select all"), usable: grid.count > 0,
                          triggered: () => grid.selectAll() }]
                ShellButton {
                    objectName: "selectRange"
                    flat: true
                    text: qsTr("Range")
                    active: grid.rangeNext
                    hint: selectBar.hints ? "R" : ""
                    action: selectRange
                }
            }
            Repeater {
                model: selectBar.groupsBefore
                delegate: PluginGroup {
                    required property var modelData
                    items: modelData
                    upward: true
                    keyLayer: "grid"
                    hints: selectBar.hints
                    // Said over the bar, where the foot's notice would cover it.
                    onComplained: historyNotice.giveWay()
                }
            }
            // As JPEG and MP4, what opens anywhere; the originals on Shift.
            MenuGroup {
                objectName: "selectCopyGroup"
                menuText: qsTr("Copy")
                upward: true
                menuKey: "Shift+C"
                keyLayer: "grid"
                section: "share"
                items: [
                    { text: qsTr("Export"), helpText: qsTr("Export JPEG"), key: "E", usable: grid.selectionCount > 0,
                      triggered: () => outgoing.exportTo(grid.selection(), true) },
                    { text: qsTr("Export originals"), helpText: qsTr("Export original"), key: "Shift+E", usable: grid.selectionCount > 0,
                      triggered: () => outgoing.exportTo(grid.selection(), false) }
                ]
                CopyButton {
                    objectName: "selectCopyButton"
                    photos: () => grid.selection()
                    enabledHere: grid.selectionCount > 0
                    flat: true
                    key: "C"
                    keyLayer: "grid"
                    section: "share"
                    hint: selectBar.hints ? "C" : ""
                }
            }
            Repeater {
                model: selectBar.groupsAfter
                delegate: PluginGroup {
                    required property var modelData
                    items: modelData
                    upward: true
                    keyLayer: "grid"
                    hints: selectBar.hints
                    // Said over the bar, where the foot's notice would cover it.
                    onComplained: historyNotice.giveWay()
                }
            }
        }
    }

    // The plugin host view collects its own actions, marks and overlays.
    PluginPlane {
        id: pluginPlane
        onCollectionChanged: {
            if (tourShown && pluginItem(tourView.item.pluginId) === null) tourView.item.close()
            tourDue.restart()
        }
        onTourStateChanged: tourDue.restart()
    }
    readonly property var pluginActions: pluginPlane?.pluginActions ?? []
    readonly property var pluginLayers: pluginPlane?.pluginLayers ?? []
    readonly property var pluginSources: pluginPlane?.pluginSources ?? ({})
    readonly property var pluginSourceList: pluginPlane?.pluginSourceList ?? []
    readonly property var pluginStacks: pluginPlane?.pluginStacks ?? []
    readonly property var pluginHidden: pluginPlane?.pluginHidden ?? []
    readonly property var pluginHiders: pluginPlane?.pluginHiders ?? []
    readonly property var pluginHelp: pluginPlane?.pluginHelp ?? []
    readonly property var coverOrder: pluginPlane?.coverOrder ?? []
    readonly property string coveringPlugin: pluginPlane?.coveringPlugin ?? ""
    readonly property bool pluginCovering: pluginPlane?.pluginCovering ?? false
    readonly property int pluginsLoaded: pluginPlane?.pluginsLoaded ?? 0
    function pluginItem(id) { return pluginPlane.pluginItem(id) }
    function collectPlugins(freshHidden, freshStacks) { pluginPlane.collectPlugins(freshHidden, freshStacks) }

    // A plugin's tour (Tour.qml), from the side panel; and by itself the
    // first time the plugin is on, once it has something to show and the
    // grid is free (a plugin turned on in the side panel: once it closes).
    // Of those on at launch, at most one by itself: with more than one not
    // seen yet, a card lists them instead (NewPlugins.qml), each once its
    // tour is ready. A plugin turned on later still shows its own.
    function startTour(id) {
        const item = pluginItem(id)
        if (!item || !item.tour || item.tour.length === 0)
            return
        side.shown = false
        tourView.active = true
        tourView.item.start(id, plugins.at(id).name, item.tour)
    }
    property bool touredThisLaunch: false
    Timer {
        id: tourDue
        interval: 1200
        onTriggered: {
            if (!autoTours || tourShown)
                return
            // Those on at launch whose tour is not seen, and of them those
            // ready; those turned on since, ready.
            const candidates = pluginPlane.tourCandidates()
            const unseen = candidates.unseen, ready = candidates.ready, turnedOn = candidates.turnedOn
            // The card takes in those ready since it showed.
            if (newPluginsShown) {
                newPluginsView.item.add(ready)
                return
            }
            if (!win.tourFree)
                return
            if (turnedOn.length > 0) {
                win.startTour(turnedOn[0])
            } else if (!win.touredThisLaunch && ready.length > 0) {
                win.touredThisLaunch = true
                if (unseen.length > 1) {
                    newPluginsView.active = true
                    newPluginsView.item.open(ready)
                } else {
                    win.startTour(ready[0])
                }
            }
        }
    }
    // Nothing in the way: no panel, the viewer or select mode, nor a source
    // in trouble saying so.
    readonly property bool tourFree: inGrid && !grid.selecting && (grid.count > 0 || grid.hiddenCount > 0) && !trouble.visible
    onTourFreeChanged: if (tourFree) tourDue.restart()

    SidePanel {
        id: side
        objectName: "sidePanel"
        health: service.health
        pluginItem: id => win.pluginItem(id)
        pluginsLoaded: win.pluginsLoaded
        onTourRequested: id => win.startTour(id)
        onGuideRequested: win.guideShown = true
        pluginSources: win.pluginSources
        onAddRequested: {
            side.shown = false
            win.showAddSource(true)
        }
        onSignInRequested: source => win.signInTo(source)
    }
    function showAddSource(shown) {
        if (shown)
            addPicker.active = true
        if (addPicker.item)
            addPicker.item.shown = shown
    }
    readonly property bool addSourceShown: addPicker.item?.shown ?? false
    Loader {
        id: addPicker
        anchors.fill: parent
        active: false
        sourceComponent: SourcePicker {
            objectName: "addSource"
            kinds: win.pluginSourceList
        }
    }
    // A plugin's own screen over the photos (iCloud's sign-in, chosen in
    // the picker): the picker gives way to it.
    onPluginCoveringChanged: if (pluginCovering) showAddSource(false)
    // A folder added, or an Apple ID signed in to: what was open for it goes.
    Connections {
        target: sources
        function onSwitched() {
            win.showAddSource(false)
            side.shown = false
        }
    }
    // The help over the card, as ? on it takes the keys.
    readonly property bool newPluginsShown: newPluginsView.item?.shown ?? false
    // The card of plugins new to tour, made the first time (a self-test's).
    function showNewPlugins(ids) {
        newPluginsView.active = true
        newPluginsView.item.open(ids)
    }
    Loader {
        id: newPluginsView
        anchors.fill: parent
        active: false
        sourceComponent: NewPlugins {
            objectName: "newPlugins"
            ready: id => win.pluginItem(id)?.tourReady ?? false
            onTourRequested: id => win.startTour(id)
        }
    }
    property bool guideShown: false
    Loader {
        id: guideView
        objectName: "pluginGuideLoader"
        anchors.fill: parent
        active: win.guideShown
        source: active ? "qrc:/PluginGuide.qml" : ""
        onLoaded: item.forceActiveFocus()
        Connections {
            target: guideView.item
            function onClosed() {
                win.guideShown = false
                if (side.shown) side.restoreFocus()
                else grid.forceActiveFocus()
            }
        }
    }
    function showHelp(shown) {
        if (shown)
            helpView.active = true
        if (helpView.item)
            helpView.item.shown = shown
    }
    readonly property bool helpShown: helpView.item?.shown ?? false
    Loader {
        id: helpView
        anchors.fill: parent
        active: false
        sourceComponent: HelpOverlay { objectName: "help"; pluginSections: win.pluginHelp }
    }
    readonly property bool tourShown: tourView.item?.shown ?? false
    Loader {
        id: tourView
        anchors.fill: parent
        active: false
        readonly property PhotoGrid photoGrid: grid
        readonly property Item sidePanel: side
        sourceComponent: Tour {
            objectName: "tour"
            grid: tourView.photoGrid
            photoViewer: win.photoViewer
            side: tourView.sidePanel
        }
    }

    // The grid's own controls, 1 shown to 0 gone: they fade as a panel or
    // the viewer comes over the photos, and back, rather than vanish.
    property real chrome: !viewerShown && !panelUp ? 1 : 0
    Behavior on chrome { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    readonly property bool chromeUp: chrome > 0

    // Something over the window that takes the keys.
    readonly property bool panelUp: helpShown || guideShown || side.shown || addSourceShown || newPluginsShown || pluginCovering
    // A tour leaves the grid's controls as they are, to point at them, but
    // takes the keys.
    readonly property bool inGrid: !viewerShown && !panelUp && !firstRun && !tourShown
    // Whose the keys are (keymap.h): the one on top, so two never share one.
    readonly property string keyLayer: tourShown ? "tour"
        : helpShown ? "help"
        : guideShown ? "guide"
        : newPluginsShown ? "newPlugins"
        : addSourceShown ? "addSource"
        : firstRun && !pluginCovering ? "firstRun"
        : side.shown ? "side"
        : pluginCovering ? "plugin"
        : orderMenu.open || (selectBar?.menuOpen ?? false) || gridMore.open || (photoViewer?.menuOpen ?? false) ? "menu"
        : viewerShown ? "viewer"
        : "grid"
    Binding { target: keymap; property: "layer"; value: win.keyLayer }

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

    // Photos and videos dragged in from elsewhere, over everything.
    DropTarget {
        objectName: "dropTarget"
        anchors.fill: parent
        name: win.placeName
        detail: win.shownKind && win.shownKind.adds ? win.shownKind.adds(win.shownSource)
                                                    : qsTr("Added there. The files here stay as they are.")
        enabled: sources.known && !win.firstRun && !win.sourcesOff
    }

    // Undo and redo (history.h): favourites, and what plugins change, as
    // Series does with a series put right. What was undone says so at the
    // foot of the window, over the viewer and a plugin's screen too, where
    // a plugin's own notices show as well.
    Command {
        id: undo
        name: "history.undo"
        text: qsTr("Undo")
        section: "pinhole"
        keys: ["Ctrl+Z"]
        keyLayer: "any"
        when: win.historyKeys
        onTriggered: history.undo()
    }
    Command {
        id: redo
        name: "history.redo"
        text: qsTr("Redo")
        section: "pinhole"
        keys: ["Ctrl+Shift+Z"]
        keyLayer: "any"
        when: win.historyKeys
        onTriggered: history.redo()
    }
    // Not while choosing a source, nor while a tour shows. (A field typed
    // into, an Apple ID, takes Ctrl Z first.)
    readonly property bool historyKeys: !addSourceShown && !tourShown
    // What was done, undone or done again, said at the foot for a while,
    // with the button that takes it back (or does it again): undo and redo
    // for a pointer, as Ctrl+Z and Ctrl+⇧Z are for the keys.
    Rectangle {
        id: historyNotice
        objectName: "historyNotice"
        property string message
        // "undo" offers Undo, "redo" Redo, "" neither.
        property string offer
        visible: opacity > 0
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: grid.selecting || win.viewerShown || win.pluginCovering ? 72 : 24
        width: noticeRow.width + 2
        height: noticeRow.height + 2
        radius: Math.min(4, theme.radius)
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
        border.width: 1
        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
        // Long enough to reach for, and longer under the pointer.
        Timer {
            id: historyNoticeShown
            interval: 4000
            onTriggered: noticeHover.hovered ? restart() : historyNotice.opacity = 0
        }
        HoverHandler { id: noticeHover }
        Shield { anchors.fill: parent; z: -1 }
        function show(message, offer) {
            historyNotice.message = message
            historyNotice.offer = offer
            opacity = 1
            historyNoticeShown.restart()
        }
        // A complaint over select mode's bar, where this stands: it goes, so
        // that can be read.
        function giveWay() {
            historyNoticeShown.stop()
            opacity = 0
        }
        Row {
            id: noticeRow
            x: 1
            y: 1
            UiText {
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: theme.fontSize
                rightPadding: theme.fontSize
                text: historyNotice.message
            }
            ShellButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: historyNotice.offer === "undo"
                flat: true
                text: qsTr("Undo")
                action: undo
            }
            ShellButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: historyNotice.offer === "redo"
                flat: true
                text: qsTr("Redo")
                action: redo
            }
        }
        Connections {
            target: history
            function onDone(name) { historyNotice.show(name, "undo") }
            function onUndone(name) { historyNotice.show(qsTr("Undone: %1").arg(name), "redo") }
            function onRedone(name) { historyNotice.show(qsTr("Redone: %1").arg(name), "undo") }
            function onNothing(undo) { historyNotice.show(undo ? qsTr("Nothing to undo") : qsTr("Nothing to redo"), "") }
            // A step that did not happen after all (a favourite refused):
            // no longer offered.
            function onDropped(name) {
                if (historyNotice.offer === "undo" && historyNotice.message === name)
                    historyNotice.giveWay()
            }
        }
        // A plugin's few words (pinhole.notify), said the same way.
        Connections {
            target: plugins
            function onNotice(id, text) { historyNotice.show(text, "") }
        }
    }

    // The window's own keys.
    Command {
        name: "app.quit"
        text: qsTr("Quit")
        section: "pinhole"
        keys: ["Q"]
        keyLayer: "any"
        noMouse: qsTr("Closed as any window is")
        noTouchpad: qsTr("Closed as any window is")
        onTriggered: Qt.quit()
    }
    Command {
        id: helpToggle
        name: "help.toggle"
        text: qsTr("Keys and gestures")
        section: "pinhole"
        keys: ["?"]
        keyLayer: "any"
        when: keymap.layer !== "tour"
        onTriggered: win.showHelp(!win.helpShown)
    }
    Command {
        name: "help.close"
        keys: ["Escape"]
        keyLayer: "help"
        mouse: qsTr("Click beside it")
        touchpad: qsTr("Tap beside it")
        onTriggered: win.showHelp(false)
    }
    // Frame times, for working on the grid: a key nobody presses by chance.
    Command {
        name: "dev.stats"
        keys: ["Ctrl+Shift+F"]
        keyLayer: "any"
        noMouse: qsTr("For working on Pinhole")
        noTouchpad: qsTr("For working on Pinhole")
        onTriggered: grid.statsVisible = !grid.statsVisible
    }
    // The side panel: sources, storage and plugins, on Tab or ",", and any
    // of the first nine sources shown on its number, from the grid or the
    // panel.
    Command {
        id: sidePanel
        name: "side.toggle"
        text: qsTr("Sources and settings")
        section: "pinhole"
        keys: keymap.layer === "side" ? [","] : ["Tab", ","]
        keyLayer: "any"
        when: (keymap.layer === "grid" || keymap.layer === "side") && sources.known
        onTriggered: side.shown = !side.shown
    }
    Repeater {
        model: 9
        delegate: Command {
            required property int index
            name: "side.show" + (index + 1)
            text: qsTr("Show a source")
            // Once in the help, for all nine.
            section: index === 0 ? "pinhole" : ""
            keys: [String(index + 1)]
            keysText: "1–9"
            keyLayer: "any"
            when: (keymap.layer === "grid" || keymap.layer === "side") && index < sources.list.length
            mouse: qsTr("Its row in the side panel")
            touchpad: qsTr("Its row in the side panel")
            pointerHelp: false
            onTriggered: sources.showAt(index)
        }
    }

    // The grid.
    // The cursor: a photo across, a row down, held a steady pace; the view
    // glides to keep it in sight.
    Command {
        name: "grid.left"
        text: qsTr("Move")
        section: "library"
        keys: ["Left"]
        keysText: "← → ↑ ↓"
        keyLayer: "grid"
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        pointerHelp: false
        onTriggered: grid.moveCursor(-1, 0, false)
    }
    Command {
        name: "grid.right"
        keys: ["Right"]
        keyLayer: "grid"
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: grid.moveCursor(1, 0, false)
    }
    Command {
        name: "grid.up"
        keys: ["Up"]
        keyLayer: "grid"
        autoRepeat: true
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        section: "library"
        text: qsTr("Scroll")
        keyHelp: false
        onTriggered: grid.moveCursor(0, -1, false)
    }
    Command {
        name: "grid.down"
        keys: ["Down"]
        keyLayer: "grid"
        autoRepeat: true
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: grid.moveCursor(0, 1, false)
    }
    // With Shift: a half screen, held faster (in select mode, ticking).
    Command {
        name: "grid.fastUp"
        text: qsTr("Faster")
        section: "library"
        keys: ["Shift+Up"]
        keysText: "⇧ ↑ ↓"
        keyLayer: "grid"
        when: !grid.selecting
        autoRepeat: true
        mouse: qsTr("Spin the wheel")
        touchpad: qsTr("Flick with two fingers")
        onTriggered: grid.scrollByKey(-1, true)
    }
    Command {
        name: "grid.fastDown"
        keys: ["Shift+Down"]
        keyLayer: "grid"
        when: !grid.selecting
        autoRepeat: true
        mouse: qsTr("Spin the wheel")
        touchpad: qsTr("Flick with two fingers")
        onTriggered: grid.scrollByKey(1, true)
    }
    Command {
        name: "grid.pageUp"
        text: qsTr("A screen up, down")
        section: "library"
        keys: ["PgUp"]
        keysText: "PgUp PgDn"
        keyLayer: "grid"
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        pointerHelp: false
        onTriggered: grid.cursorPages(-1)
    }
    Command {
        name: "grid.pageDown"
        keys: ["PgDown"]
        keyLayer: "grid"
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: grid.cursorPages(1)
    }
    Command {
        name: "grid.oldest"
        text: qsTr("Oldest")
        section: "library"
        keys: ["Home"]
        keyLayer: "grid"
        mouse: qsTr("Wheel, up to the top")
        touchpad: qsTr("Two fingers, up to the top")
        onTriggered: grid.cursorToStart()
    }
    Command {
        id: newest
        name: "grid.newest"
        text: qsTr("Newest")
        section: "library"
        keys: ["N", "End"]
        // End works too, as in any list; the button names N alone.
        keysText: "N"
        keyLayer: "grid"
        when: grid.count > 0
        onTriggered: grid.cursorToEnd()
    }
    Command {
        id: gridZoomIn
        name: "grid.zoomIn"
        text: qsTr("Zoom in, out")
        section: "library"
        keys: ["+", "="]
        keyLayer: "grid"
        mouse: qsTr("Thumb wheel, or − +")
        touchpad: qsTr("Pinch, or − +")
        keysText: "+ −"
        onTriggered: grid.zoomStep(-1)
    }
    Command {
        id: gridZoomOut
        name: "grid.zoomOut"
        keys: ["-"]
        keyLayer: "grid"
        mouse: qsTr("Thumb wheel, or − +")
        touchpad: qsTr("Pinch, or − +")
        onTriggered: grid.zoomStep(1)
    }
    Command {
        name: "grid.open"
        text: qsTr("Open")
        section: "library"
        keys: ["Return", "Enter"]
        keysText: "Space ⏎"
        keyLayer: "grid"
        when: !trouble.visible
        mouse: qsTr("Click a photo")
        touchpad: qsTr("Tap a photo")
        onTriggered: win.openCurrent()
    }
    // Space opens too, but ticks in select mode.
    Command {
        name: "grid.space"
        text: qsTr("Tick")
        section: "select"
        keys: ["Space"]
        keyLayer: "grid"
        when: !trouble.visible
        mouse: qsTr("Click a photo")
        touchpad: qsTr("Tap a photo")
        keysText: "Space"
        pointerHelp: false
        onTriggered: grid.selecting ? grid.tickCursor() : win.openCurrent()
    }
    Command {
        id: selectRange
        name: "select.range"
        text: qsTr("Range: the next photo ends it")
        keys: ["R"]
        keyLayer: "grid"
        when: grid.selecting
        onTriggered: grid.rangeNext = !grid.rangeNext
    }
    Command {
        name: "select.rangeUp"
        text: qsTr("Tick a range")
        section: "select"
        keys: ["Shift+Up"]
        keysText: "⇧ ← → ↑ ↓"
        keyLayer: "grid"
        when: grid.selecting
        autoRepeat: true
        mouse: qsTr("Drag across, or ⇧ click")
        touchpad: qsTr("Three fingers, or Range")
        onTriggered: grid.moveCursor(0, -1, true)
    }
    Command {
        name: "select.rangeDown"
        keys: ["Shift+Down"]
        keyLayer: "grid"
        when: grid.selecting
        autoRepeat: true
        mouse: qsTr("Drag across")
        touchpad: qsTr("Drag with three fingers, or Range")
        onTriggered: grid.moveCursor(0, 1, true)
    }
    Command {
        name: "select.rangeLeft"
        keys: ["Shift+Left"]
        keyLayer: "grid"
        when: grid.selecting
        autoRepeat: true
        mouse: qsTr("Drag across")
        touchpad: qsTr("Drag with three fingers, or Range")
        onTriggered: grid.moveCursor(-1, 0, true)
    }
    Command {
        name: "select.rangeRight"
        keys: ["Shift+Right"]
        keyLayer: "grid"
        when: grid.selecting
        autoRepeat: true
        mouse: qsTr("Drag across")
        touchpad: qsTr("Drag with three fingers, or Range")
        onTriggered: grid.moveCursor(1, 0, true)
    }
    Command {
        name: "select.leave"
        keys: ["Escape"]
        keyLayer: "grid"
        when: grid.selecting && !trouble.visible
        mouse: qsTr("Done")
        touchpad: qsTr("Done")
        onTriggered: grid.selecting = false
    }

    // Out of the app, from the grid or the viewer: the selection, else the
    // photo open, else the one under the pointer.
    Command {
        name: "grid.copy"
        text: qsTr("Copy")
        section: "share"
        keys: ["C"]
        keyLayer: "grid"
        when: win.inGrid && !grid.selecting && grid.count > 0
        mouse: qsTr("Viewer, select mode")
        touchpad: qsTr("Viewer, select mode")
        onTriggered: outgoing.copy(win.chosen())
    }
    Command {
        name: "share.copy"
        mouse: qsTr("Viewer, select mode")
        touchpad: qsTr("Viewer, select mode")
        text: qsTr("Copy")
        section: "share"
        keys: ["Ctrl+C"]
        keyLayer: "chrome"
        keyHelp: false
        onTriggered: outgoing.copy(win.chosen())
    }
    Command {
        name: "share.export"
        mouse: qsTr("Viewer, select mode")
        touchpad: qsTr("Viewer, select mode")
        text: qsTr("Export JPEG")
        section: "share"
        keys: ["Ctrl+E"]
        keyLayer: "chrome"
        keyHelp: false
        onTriggered: outgoing.exportTo(win.chosen(), true)
    }
    Command {
        name: "share.exportOriginal"
        mouse: qsTr("Viewer, select mode")
        touchpad: qsTr("Viewer, select mode")
        text: qsTr("Export original")
        section: "share"
        keys: ["Ctrl+Shift+E"]
        keyLayer: "chrome"
        keyHelp: false
        onTriggered: outgoing.exportTo(win.chosen(), false)
    }

    // What only a pointer does, for the help's mouse and touchpad tabs.
    Command {
        name: "gesture.stop"
        text: qsTr("Stop a coast")
        section: "library"
        bound: false
        keyHelp: false
        noKeyboard: qsTr("The keys move it a step at a time")
        mouse: qsTr("Click")
        touchpad: qsTr("Rest a finger, or tap two")
    }
    Command {
        name: "gesture.drag"
        text: qsTr("Into another app")
        section: "share"
        bound: false
        keyHelp: false
        noKeyboard: qsTr("Ctrl\u00a0C, then paste there")
        mouse: qsTr("Drag photos out")
        touchpad: qsTr("Drag out with three fingers")
    }
    Command {
        name: "gesture.drop"
        text: qsTr("Add to the library")
        section: "share"
        bound: false
        keyHelp: false
        noKeyboard: qsTr("Files come in by being dropped")
        mouse: qsTr("Drop files on it")
        touchpad: qsTr("Drop files on it")
    }
    Command {
        name: "gesture.close"
        text: qsTr("Back to the library")
        section: "photo"
        bound: false
        keyHelp: false
        noKeyboard: qsTr("Esc")
        mouse: qsTr("Drag up or down, Back")
        touchpad: qsTr("Swipe up or down, pinch in")
    }
    Command {
        name: "gesture.play"
        text: qsTr("Play, pause")
        section: "video"
        bound: false
        keyHelp: false
        noKeyboard: qsTr("Space")
        mouse: qsTr("Click the video")
        touchpad: qsTr("Tap the video")
    }
    Command {
        name: "gesture.seek"
        text: qsTr("Seek")
        section: "video"
        bound: false
        keyHelp: false
        noKeyboard: qsTr("[ ]")
        mouse: qsTr("The bar at the foot")
        touchpad: qsTr("The bar at the foot")
    }

    // The viewer. ← and →, with Shift or not, are its own: it reads them
    // pressed and let go, to move on while one is held.
    Binding { target: viewer; property: "takesInput"; value: keymap.layer === "viewer" }
    Command {
        name: "viewer.space"
        text: qsTr("Play, pause; close a photo")
        section: "video"
        keys: ["Space", "Return"]
        keyLayer: "viewer"
        mouse: qsTr("Click the video")
        touchpad: qsTr("Tap the video")
        // Space plays and pauses a video; on a photo it closes, like Quick Look.
        pointerHelp: false
        onTriggered: if (!photoViewer.togglePlay()) photoViewer.closeViewer()
    }
    Command {
        name: "video.back"
        text: qsTr("10 seconds back, on")
        section: "video"
        keys: ["["]
        keysText: "[ ]"
        keyLayer: "viewer"
        when: viewer.video
        mouse: qsTr("Click the bar at the foot")
        touchpad: qsTr("Tap the bar at the foot")
        pointerHelp: false
        onTriggered: photoViewer.skip(-10)
    }
    Command {
        name: "video.on"
        keys: ["]"]
        keyLayer: "viewer"
        when: viewer.video
        mouse: qsTr("Click the bar at the foot")
        touchpad: qsTr("Tap the bar at the foot")
        onTriggered: photoViewer.skip(10)
    }
    // The arrows are the viewer's own (viewer.cpp): listed here.
    Command {
        name: "viewer.step"
        text: qsTr("Previous, next")
        section: "photo"
        keys: ["Left", "Right"]
        keysText: "← →"
        keyLayer: "viewer"
        bound: false
        mouse: qsTr("Thumb wheel, a side, drag")
        touchpad: qsTr("Swipe with two fingers")
    }
    Command {
        name: "viewer.fast"
        text: qsTr("Faster")
        section: "photo"
        keys: ["Shift+Left", "Shift+Right"]
        keysText: "⇧ ← →"
        keyLayer: "viewer"
        bound: false
        mouse: qsTr("The strip at the foot")
        touchpad: qsTr("The strip at the foot")
    }
    Command {
        name: "viewer.pan"
        text: qsTr("Look around, zoomed in")
        section: "photo"
        keys: ["Left", "Right", "Up", "Down"]
        keysText: "← → ↑ ↓"
        keyLayer: "viewer"
        bound: false
        mouse: qsTr("Drag")
        touchpad: qsTr("Two fingers")
    }
    Command {
        name: "viewer.first"
        text: qsTr("First, last")
        section: "photo"
        keys: ["Home"]
        keysText: "Home End"
        keyLayer: "viewer"
        mouse: qsTr("The strip at the foot")
        touchpad: qsTr("The strip at the foot")
        pointerHelp: false
        onTriggered: viewer.jumpTo(0)
    }
    Command {
        name: "viewer.last"
        keys: ["End"]
        keyLayer: "viewer"
        mouse: qsTr("The strip at the foot")
        touchpad: qsTr("The strip at the foot")
        onTriggered: viewer.jumpTo(viewer.count - 1)
    }
    Command {
        name: "viewer.pageBack"
        text: qsTr("Ten back, on")
        section: "photo"
        keys: ["PgUp"]
        keysText: "PgUp PgDn"
        keyLayer: "viewer"
        mouse: qsTr("The strip at the foot")
        touchpad: qsTr("The strip at the foot")
        pointerHelp: false
        onTriggered: viewer.goTo(viewer.place - 10)
    }
    Command {
        name: "viewer.pageOn"
        keys: ["PgDown"]
        keyLayer: "viewer"
        mouse: qsTr("The strip at the foot")
        touchpad: qsTr("The strip at the foot")
        onTriggered: viewer.goTo(viewer.place + 10)
    }
    Command {
        name: "viewer.zoomIn"
        text: qsTr("Zoom in, out")
        section: "photo"
        keys: ["+", "="]
        keysText: "+ −"
        keyLayer: "viewer"
        mouse: qsTr("Wheel")
        touchpad: qsTr("Pinch")
        onTriggered: viewer.zoomStep(1)
    }
    Command {
        name: "viewer.zoomOut"
        keys: ["-"]
        keyLayer: "viewer"
        mouse: qsTr("Wheel")
        touchpad: qsTr("Pinch")
        onTriggered: viewer.zoomStep(-1)
    }
    Command {
        name: "viewer.fit"
        text: qsTr("Fit")
        section: "photo"
        keys: ["0"]
        keyLayer: "viewer"
        mouse: qsTr("Double-click")
        touchpad: qsTr("Double-tap")
        onTriggered: viewer.resetZoom()
    }
}
