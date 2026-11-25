import QtQuick
import QtQml
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
    // ⇧ and the button's key; with none shown, the group's first key, so
    // the menu's way by the keys is there whatever shows (its items
    // without a key of their own are done through it).
    readonly property var keyed: action && action.key !== "" ? action : items.find(a => a.key !== "") ?? null
    // None where one of them has that key itself (Turn and crop's Turn
    // back, ⇧ R): its own keys are the way to each, and the ▾ the menu's.
    readonly property string shifted: keyed && !keyed.key.startsWith("Shift+") ? "Shift+" + keyed.key : ""
    menuKey: shifted !== "" && !items.some(a => a.key === shifted) ? shifted : ""

    // The button stands for each of them, for a tour to light.
    function claim() {
        for (const a of items)
            a.button = groupButton
    }
    onItemsChanged: claim()
    // Back from More (MoreGroup.qml), it stands for them again.
    onVisibleChanged: if (visible) claim()
    Component.onCompleted: claim()
    Component.onDestruction: {
        for (const a of items)
            if (a.button === groupButton)
                a.button = null
    }

    // What any of them could not do (PluginAction.complain), under the
    // button while it shows; folded, More ▾ says it.
    Instantiator {
        model: group.items
        delegate: Connections {
            required property var modelData
            target: modelData
            function onComplained(text) {
                if (group.visible)
                    group.complain(text)
            }
        }
    }

    ShellButton {
        id: groupButton
        flat: true
        visible: group.action !== null
        usable: group.action?.usable ?? true
        active: group.action?.active ?? false
        labelColor: {
            const name = group.action?.textColour ?? ""
            if (name === "")
                return theme.foreground
            return ["red", "green", "yellow", "blue", "magenta", "cyan", "accent"].indexOf(name) >= 0 ? theme[name] : name
        }
        busy: group.action?.busy ?? false
        busyDelay: group.action?.busyDelay ?? 1000
        text: group.action?.text ?? ""
        hint: group.hints ? KeyNames.nameOf(group.action?.key ?? "") : ""
        onTriggered: group.press(group.action)
        onHoveredChanged: if (group.action) group.action.hovered = hovered
        Component.onDestruction: if (hovered && group.action) group.action.hovered = false
    }
}
