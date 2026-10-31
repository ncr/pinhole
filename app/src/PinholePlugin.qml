import QtQuick

// The root of a plugin's entry point (docs/plugins.md). It covers the
// window, above the photos and the viewer, and below Pinhole's own panels;
// what it shows there is its own. Pinhole draws its actions as its own
// buttons, in the places they name, and its settings in the side panel.
Item {
    // What Pinhole gives the plugin (plugins.h: PluginHost).
    property var pinhole
    // Buttons, each a PluginAction.
    property list<PluginAction> actions
    // Under the plugin's switch in the side panel, while it is on.
    property Component settings
    // Over the photos and taking the keys: the grid's controls fade and its
    // keys rest, as for Pinhole's own panels.
    property bool covering: false
    // Its keys, as the help (?) lists them: [["G", "Series"], ...].
    property var help: []
    // Photos shown in the grid as one card, each a list of library
    // positions: the card stands where the first is and shows it, with the
    // mark for several photos. Opening it opens that photo.
    property var stacks: []
    // Photos not shown in the grid at all, library positions: no cell, out
    // of any stack, and passed over by the viewer's ← and → and its strip.
    property var hidden: []
    // A few steps that show what it does, on the photos and its buttons as
    // they are (Tour.qml): shown by itself the first time it is on, once
    // `tourReady`, and again from the side panel.
    property var tour: []
    property bool tourReady: true
}
