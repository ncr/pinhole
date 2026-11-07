import QtQuick
import QtQml

// The plugins' actions folded into one button, More ▾, where the room they
// would take is not there (the viewer's bar, the grid's top right): its
// menu lists each that shows, with its key. Their keys work as they do
// unfolded, and a group's ⇧ key opens the menu on that group's row.
MenuGroup {
    id: more
    // The plugins' groups, each a list of PluginActions (ActionBar.groupsOf).
    property var groups: []
    // Over the photos, as the grid's buttons are, rather than in a bar.
    property bool floating: false

    items: {
        const list = []
        for (const group of groups)
            for (const action of group)
                list.push(action)
        return list
    }
    arrowShown: false
    keyedMenu: true
    menuText: qsTr("More")

    // The button stands for each of them while it shows, for a tour to
    // light (as PluginGroup's does unfolded).
    function claim() {
        if (visible)
            for (const a of items)
                a.button = moreButton
    }
    onVisibleChanged: claim()
    onItemsChanged: claim()
    Component.onDestruction: {
        for (const a of items)
            if (a.button === moreButton)
                a.button = null
    }

    // What one of them could not do (PluginAction.complain), under More.
    Instantiator {
        model: more.items
        delegate: Connections {
            required property var modelData
            target: modelData
            function onComplained(text) {
                if (more.visible)
                    more.complain(text)
            }
        }
    }

    ShellButton {
        id: moreButton
        objectName: "moreButton"
        flat: !more.floating
        floating: more.floating
        // ▾ where a key would be, so an arc turning takes its place and
        // the word stays.
        text: qsTr("More")
        hint: "▾"
        Accessible.expandable: true
        Accessible.expanded: more.open
        // What one of them is doing shows on it folded: lit, or turning.
        lit: more.open
        active: more.items.some(a => a.shown && a.active)
        busy: more.items.some(a => a.shown && a.busy)
        onTriggered: more.toggle(false)
    }

    // Each folded group's ⇧ key (PluginGroup's menuKey), opening the menu
    // on its row.
    function keyedOf(group) {
        const shown = group.find(a => a.shown)
        const keyed = shown && shown.key !== "" ? shown : group.find(a => a.key !== "")
        return keyed && !keyed.key.startsWith("Shift+") ? keyed : null
    }
    Instantiator {
        model: more.groups.filter(group => group.length > 1 && more.keyedOf(group) !== null)
        delegate: Command {
            required property var modelData
            // Its actions shown or not one at a time, the group may have
            // none to go by a moment before the model follows.
            readonly property var keyed: more.keyedOf(modelData)
            name: "more.open." + (keyed?.key ?? "")
            keys: keyed ? ["Shift+" + keyed.key] : []
            keyLayer: more.keyLayer
            usable: keyed !== null && more.visible && modelData.some(a => a.shown)
            buttons: 1
            onTriggered: more.openAt(modelData.find(a => a.shown))
        }
    }
}
