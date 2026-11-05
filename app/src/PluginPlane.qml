import QtQuick
import QtQuick.Window

// Owns plugin lifetime, aggregated actions and photo presentation state.
// It knows no application panels: the shell decides how tours are shown.
Item {
    id: root
    objectName: "pluginPlane"
    anchors.fill: parent
    signal collectionChanged()
    signal tourStateChanged()

    Repeater {
        id: pluginLayer
        model: plugins.ids
        delegate: Loader {
            id: pluginLoader
            required property string modelData
            // Whose its Commands are (Command.qml).
            readonly property string pluginId: modelData
            readonly property var info: (plugins.revision, plugins.at(modelData))
            // Once the library has been read (plugins.ready).
            readonly property string main: info.enabled && plugins.ready ? info.main : ""
            // Turned on in the side panel since the launch: its tour
            // shows by itself whatever showed at launch (tourDue).
            readonly property bool switchedOn: info.enabled === true
            property bool turnedOn: false
            onSwitchedOnChanged: if (switchedOn && completed) turnedOn = true
            anchors.fill: parent
            // One plugin over the photos at a time: the one covering
            // above the rest, and while it does, the others (an overlay
            // as Info's, one that covered before it) unseen beneath.
            z: root.coveringPlugin === modelData ? 1 : 0
            // Away while a photo it opened to come back from shows (API 11):
            // not covering, so the viewer has the keys. What it shows over
            // the photo (Info's details) stays; its view hides by itself.
            readonly property bool away: info.host ? info.host.away === true : false
            onAwayChanged: root.collectPlugins()
            visible: root.coveringPlugin === "" || root.coveringPlugin === modelData
            property bool completed: false
            function load() {
                const began = Date.now()
                main !== "" ? setSource(main, { pinhole: info.host }) : source = ""
                if (main !== "") plugins.startupLoaded(modelData, Date.now() - began)
            }
            onMainChanged: if (completed) load()
            Component.onCompleted: { completed = true; load() }
            onLoaded: {
                // Its marks straight to the grid (plugins.h), not
                // gathered with the rest.
                plugins.setMarks(modelData, item.marks || [])
                root.collectPlugins([modelData], [modelData])
            }
            onStatusChanged: {
                // What kept it from loading, said under it in the side
                // panel.
                if (status === Loader.Error)
                    plugins.loadFailed(modelData)
                if (status === Loader.Null || status === Loader.Error) {
                    plugins.setMarks(modelData, [])
                    root.collectPlugins()
                }
            }
            Connections {
                target: pluginLoader.item
                ignoreUnknownSignals: true
                function onCoveringChanged() { root.collectPlugins() }
                function onActionsChanged() { root.collectPlugins() }
                function onHelpChanged() { root.collectPlugins() }
                function onViewerLayerChanged() { root.collectPlugins() }
                function onSourcesChanged() { root.collectPlugins() }
                function onStacksChanged() { root.collectPlugins([], [modelData]) }
                function onHiddenChanged() { root.collectPlugins([modelData], []) }
                function onMarksChanged() { plugins.setMarks(pluginLoader.modelData, pluginLoader.item.marks) }
                function onTourReadyChanged() { root.tourStateChanged() }
            }
        }
    }
    Connections {
        target: plugins
        // The library read again: what each plugin hides and stacks,
        // where those photos are now, until it says again.
        function onPositionsMoved() { root.collectPlugins() }
        // Those on at launch, gathered once they all have loaded, before
        // the first frame: each gathered as it loaded rebuilt every
        // button and list again and again, a second in all.
        function onAllLoaded() {
            const loaded = root.loadedMeanwhile
            root.loadedMeanwhile = []
            root.collectPlugins(loaded, loaded)
        }
    }
    // The plugins that loaded, or gave what they hide, as all loaded.
    property var loadedMeanwhile: []
    // What the plugins loaded add: their buttons, their keys for the help,
    // the photos they stack into one card or hide, and whether one is over
    // the photos.
    property var pluginActions: []
    // Their layers over a photo shown full window: { plugin, component }.
    property var pluginLayers: []
    // Each kind of source as the plugin that brings it shows it
    // (PluginSource), by kind; and the same in the service's order of kinds
    // (iCloud's, folders', then those of one's own).
    property var pluginSources: ({})
    readonly property var pluginSourceList: {
        const order = sources.kinds.map(k => k.kind)
        const place = kind => order.indexOf(kind) < 0 ? order.length : order.indexOf(kind)
        return Object.keys(pluginSources).map((kind, i) => ({ kind: kind, i: i }))
            .sort((a, b) => place(a.kind) - place(b.kind) || a.i - b.i)
            .map(k => pluginSources[k.kind])
    }
    property var pluginStacks: []
    property var pluginHidden: []
    // The names of the plugins hiding any.
    property var pluginHiders: []
    property var pluginHelp: []
    // The plugins covering, in the order they came; the last is over the
    // photos (plugins.coveredBy), its keys the "plugin" layer's.
    property var coverOrder: []
    readonly property string coveringPlugin: coverOrder.length > 0 ? coverOrder[coverOrder.length - 1] : ""
    readonly property bool pluginCovering: coveringPlugin !== ""
    Binding { target: plugins; property: "covering"; value: root.coverOrder }
    // Those covering now, and the order: those still covering in the order
    // they came, then any new.
    property var coveringNow: []
    function orderCovering() {
        const order = coverOrder.filter(id => coveringNow.indexOf(id) >= 0)
        for (const id of coveringNow)
            if (order.indexOf(id) < 0)
                order.push(id)
        if (order.join(" ") !== coverOrder.join(" "))
            coverOrder = order
    }
    Binding { target: keymap; property: "plugin"; value: root.coveringPlugin }
    // The plugin over the photos has the keys: what it shows, unless it gave
    // them to something of its own first (a field, a list), else its root,
    // so the keys never stay with the grid beneath it.
    onCoveringPluginChanged: if (coveringPlugin !== "") Qt.callLater(() => {
        const item = pluginItem(coveringPlugin)
        if (!item || !item.Window.window) return
        for (let at = item.Window.window.activeFocusItem; at; at = at.parent)
            if (at === item) return
        item.forceActiveFocus()
    })
    property int pluginsLoaded: 0
    function pluginItem(id) {
        for (let i = 0; i < pluginLayer.count; ++i) {
            const loader = pluginLayer.itemAt(i)
            if (loader && loader.modelData === id)
                return loader.item
        }
        return null
    }
    // `freshHidden`, `freshStacks`: the plugins whose hidden photos or
    // stacks were just given (or which just loaded); the others' follow
    // their photos through a reload until they give theirs again
    // (plugins.hiddenOf, plugins.stacksOf).
    // A plugin action's key that is another's too, in the same place, or
    // one of Pinhole's own there: two shortcuts on one key, neither works.
    // Said under the plugin, as a problem.
    function findClashes(actions, owners) {
        const layersOf = place => place === "viewer" ? ["viewer", "chrome", "any"] : ["grid", "chrome", "any"]
        // Pinhole's own: written in its own files (qrc:), not a plugin's, and
        // not the Command a button or a plugin's action makes for its key.
        const own = keymap.actions().filter(c => c.bound !== false && c.keys && c.keys.length
            && String(c.file).startsWith("qrc:") && !/\/(PluginAction|ShellButton|Choices|MenuGroup|MoreGroup|MenuRow)\.qml$/.test(String(c.file)))
        const taken = {}
        for (let i = 0; i < actions.length; ++i) {
            const action = actions[i]
            if (!action.key) continue
            const key = keymap.portable(action.key)
            const place = action.place === "viewer" ? "viewer" : "grid"
            const clash = own.find(c => layersOf(place).indexOf(c.keyLayer) >= 0
                && c.keys.some(k => keymap.portable(k) === key))
            const other = taken[place + " " + key]
            if (clash)
                plugins.keyClashed(owners[i], qsTr("%1 is Pinhole's own key for %2 there: neither works. Give \"%3\" another key")
                    .arg(action.key).arg(clash.text || clash.name).arg(action.text))
            else if (other && other.owner !== owners[i])
                plugins.keyClashed(owners[i], qsTr("%1 is %2's key there too: neither works. Give \"%3\" another key")
                    .arg(action.key).arg(plugins.at(other.owner).name || other.owner).arg(action.text))
            else
                taken[place + " " + key] = { owner: owners[i] }
        }
    }
    function collectPlugins(freshHidden, freshStacks) {
        freshHidden = freshHidden || []
        freshStacks = freshStacks || []
        if (plugins.loadingAll) {
            for (const id of freshHidden.concat(freshStacks))
                if (loadedMeanwhile.indexOf(id) < 0)
                    loadedMeanwhile.push(id)
            return
        }
        const actions = [], owners = [], help = [], covering = [], layers = [], kinds = {}
        let stacks = [], hidden = [], hiders = []
        for (let i = 0; i < pluginLayer.count; ++i) {
            const loader = pluginLayer.itemAt(i)
            const item = loader ? loader.item : null
            if (!item)
                continue
            for (let k = 0; k < (item.actions ? item.actions.length : 0); ++k) {
                actions.push(item.actions[k])
                owners.push(loader.modelData)
            }
            if (item.help && item.help.length > 0)
                help.push({ plugin: loader.modelData, keys: item.help })
            if (item.viewerLayer)
                layers.push({ plugin: loader.modelData, component: item.viewerLayer })
            // Only the kinds its manifest names (Plugins::at's sources).
            for (let k = 0; k < (item.sources ? item.sources.length : 0); ++k) {
                const source = item.sources[k]
                if ((loader.info.sources || []).indexOf(source.kind) < 0)
                    plugins.sourceRefused(loader.modelData, source.kind)
                else if (!kinds[source.kind])
                    kinds[source.kind] = source
            }
            const stacked = plugins.stacksOf(loader.modelData, item.stacks || [], freshStacks.indexOf(loader.modelData) >= 0)
            if (stacked.length > 0)
                stacks = stacks.concat(stacked)
            const away = plugins.hiddenOf(loader.modelData, item.hidden || [], freshHidden.indexOf(loader.modelData) >= 0)
            if (away.length > 0) {
                hidden = hidden.concat(away)
                hiders.push(loader.info.name)
            }
            if (item.covering === true && !loader.away)
                covering.push(loader.modelData)
        }
        findClashes(actions, owners)
        // By their order, then as the plugins list them (V4's sort is not
        // stable: by position too).
        pluginActions = actions.map((a, i) => ({ a: a, i: i }))
            .sort((x, y) => (x.a.order || 0) - (y.a.order || 0) || x.i - y.i)
            .map(x => x.a)
        // Made again only when one comes or goes: a photo's layers stay.
        if (layers.length !== pluginLayers.length
                || layers.some((l, i) => l.plugin !== pluginLayers[i].plugin || l.component !== pluginLayers[i].component))
            pluginLayers = layers
        // Set only when one comes or goes: the side panel's rows follow it.
        const kindNames = Object.keys(kinds)
        if (kindNames.length !== Object.keys(pluginSources).length || kindNames.some(k => pluginSources[k] !== kinds[k]))
            pluginSources = kinds
        pluginHelp = help
        pluginStacks = stacks
        pluginHidden = hidden
        pluginHiders = hiders
        // One that came covers at once; one that stopped leaves the order
        // a moment later, once what stopped it is done: a photo it hands
        // over meanwhile (openPhoto, reveal) is then the top one's, and
        // closes those beneath it for good (plugins.h), before the next
        // beneath would show.
        coveringNow = covering
        if (coverOrder.some(id => covering.indexOf(id) < 0)) {
            const order = coverOrder.slice()
            for (const id of covering)
                if (order.indexOf(id) < 0)
                    order.push(id)
            if (order.length !== coverOrder.length)
                coverOrder = order
            Qt.callLater(root.orderCovering)
        } else {
            orderCovering()
        }
        ++pluginsLoaded
        collectionChanged()
    }


    // Any plugin asks for SDR (PinholePlugin.holdSdr).
    readonly property bool holdSdr: {
        let hold = false
        for (let i = 0; i < pluginLayer.count; ++i) {
            const loader = pluginLayer.itemAt(i)
            if (loader && loader.item && loader.item.holdSdr === true)
                hold = true
        }
        return hold
    }

    function tourCandidates() {
        const unseen = [], ready = [], turnedOn = []
        for (let i = 0; i < pluginLayer.count; ++i) {
            const loader = pluginLayer.itemAt(i)
            const item = loader ? loader.item : null
            if (!item || !item.tour || item.tour.length === 0 || loader.info.toured)
                continue
            if (loader.turnedOn) {
                if (item.tourReady)
                    turnedOn.push(loader.modelData)
                continue
            }
            unseen.push(loader.modelData)
            if (item.tourReady)
                ready.push(loader.modelData)
        }
        return { unseen: unseen, ready: ready, turnedOn: turnedOn }
    }
}
