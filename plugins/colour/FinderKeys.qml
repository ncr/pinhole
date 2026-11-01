import QtQuick
import Pinhole.Plugin

// The finder's keys, while it is open (Command.qml): here, not in
// Finder.qml, so that they are in the help before it is first made.
Item {
    id: finderKeys
    // The finder, once it has been opened; null before.
    required property var finder
    readonly property bool shown: finder !== null && finder.shown

    Command {
        name: "colour.close"
        keys: ["Escape"]
        keyLayer: "plugin"
        when: finderKeys.shown
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: finderKeys.finder.close()
    }
    Command {
        name: "colour.close.h"
        keys: ["H"]
        keyLayer: "plugin"
        when: finderKeys.shown
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: finderKeys.finder.close()
    }
    Command {
        name: "colour.hue"
        text: qsTr("Hue round the wheel")
        section: "Colours"
        keys: ["Left"]
        keysText: "← →"
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderKeys.finder.turn(10)
    }
    Command {
        name: "colour.hueBack"
        keys: ["Right"]
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderKeys.finder.turn(-10)
    }
    Command {
        name: "colour.stronger"
        text: qsTr("Stronger, greyer")
        section: "Colours"
        keys: ["Up"]
        keysText: "↑ ↓"
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderKeys.finder.strengthen(0.05)
    }
    Command {
        name: "colour.greyer"
        keys: ["Down"]
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderKeys.finder.strengthen(-0.05)
    }
    Command {
        name: "colour.darker"
        text: qsTr("Darker, lighter")
        section: "Colours"
        keys: ["["]
        keysText: "[ ]"
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the strip")
        touchpad: qsTr("Tap the strip")
        onTriggered: finderKeys.finder.lighten(-0.04)
    }
    Command {
        name: "colour.lighter"
        keys: ["]"]
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the strip")
        touchpad: qsTr("Tap the strip")
        onTriggered: finderKeys.finder.lighten(0.04)
    }
    Command {
        name: "colour.first"
        text: qsTr("Open the closest")
        section: "Colours"
        keys: ["Return", "Enter"]
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "find" && finderKeys.finder.results.length > 0
        mouse: qsTr("Click a photo")
        touchpad: qsTr("Tap a photo")
        onTriggered: finderKeys.finder.open(finderKeys.finder.results[0])
    }
    // = as well as +, as for the grid's zoom.
    Command {
        name: "colour.bigger"
        keys: ["="]
        keyLayer: "plugin"
        when: finderKeys.shown && finderKeys.finder.mode === "wall" && finderKeys.finder.canGrow
        mouse: qsTr("The + button")
        touchpad: qsTr("Pinch")
        onTriggered: finderKeys.finder.grow()
    }
}
