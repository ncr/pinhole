import QtQuick
import Pinhole

// A bar of actions as one Omarchy card: the theme's background with a
// hairline in the text colour, flat buttons in it, and a hairline between
// groups (MenuGroup.qml), each a button with the rest of its group in a
// menu under ▾.
Rectangle {
    id: bar
    default property alias content: row.data
    // A group's menu is open: it has the arrows, ⏎ and Esc.
    readonly property bool menuOpen: {
        for (let i = 0; i < row.children.length; ++i)
            if (row.children[i].isGroup === true && row.children[i].open)
                return true
        return false
    }

    function closeMenus() {
        for (let i = 0; i < row.children.length; ++i)
            if (row.children[i].isGroup === true)
                row.children[i].open = false
    }
    // What its buttons said they could not do, gone (another photo shown).
    function hush() {
        for (let i = 0; i < row.children.length; ++i)
            if (row.children[i].isGroup === true)
                row.children[i].hush()
    }
    // Groups of a place's PluginActions, in their order: those with the same
    // `group` together, each other one alone.
    function groupsOf(actions) {
        const groups = [], named = {}
        for (const a of actions) {
            if (a.group === "") {
                groups.push([a])
            } else if (named[a.group] === undefined) {
                named[a.group] = groups.length
                groups.push([a])
            } else {
                groups[named[a.group]].push(a)
            }
        }
        return groups
    }

    // In a band of the app rather than over the photos: its buttons alone,
    // the band their card.
    property bool bare: false

    width: row.width + 2
    height: row.height + 2
    radius: Math.min(4, theme.radius)
    color: bare ? "transparent" : theme.background
    border.color: theme.foreground
    border.width: bare ? 0 : 1
    // Between and around its buttons nothing reaches what is beneath (a
    // photo in select mode, the photo in the viewer).
    Shield { anchors.fill: parent; z: -1 }

    Row {
        id: row
        x: 1
        y: 1
    }
}
