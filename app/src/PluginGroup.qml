import QtQuick
import "keys.js" as KeyNames

// A group of a plugin's actions in an ActionBar: the first one shown is the
// button, the rest are in its menu, opened with ⇧ and the button's key.
MenuGroup {
    id: group
    // Keys beside the labels.
    property bool hints: true
    readonly property var action: primary >= 0 ? items[primary] : null
    primary: items.findIndex(a => a.shown)
    visible: primary >= 0
    menuKey: action && action.key !== "" && !action.key.startsWith("Shift+") ? "Shift+" + action.key : ""

    // The button stands for each of them, for a tour to light.
    function claim() {
        for (const a of items)
            a.button = groupButton
    }
    onItemsChanged: claim()
    Component.onCompleted: claim()
    Component.onDestruction: {
        for (const a of items)
            if (a.button === groupButton)
                a.button = null
    }

    ShellButton {
        id: groupButton
        flat: true
        visible: group.action !== null
        usable: group.action?.usable ?? true
        active: group.action?.active ?? false
        busy: group.action?.busy ?? false
        text: group.action?.text ?? ""
        hint: group.hints ? KeyNames.nameOf(group.action?.key ?? "") : ""
        onTapped: group.press(group.action)
        onHoveredChanged: if (group.action) group.action.hovered = hovered
        Component.onDestruction: if (hovered && group.action) group.action.hovered = false
    }
}
