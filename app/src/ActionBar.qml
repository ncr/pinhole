import QtQuick

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

    width: row.width + 2
    height: row.height + 2
    radius: Math.min(4, theme.radius)
    color: theme.background
    border.color: theme.foreground
    border.width: 1

    Row {
        id: row
        x: 1
        y: 1
    }
}
