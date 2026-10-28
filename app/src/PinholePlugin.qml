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
}
