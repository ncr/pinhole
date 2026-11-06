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
    // keys rest, as for Pinhole's own panels. One plugin at a time, the last
    // to start covering: the others wait beneath, unseen and without keys
    // (pinhole.coveredBy says whose it is). By itself while one of its
    // PluginViews is open.
    property bool covering: openViews > 0
    // How many of its PluginViews are open (they count themselves).
    property int openViews: 0
    // Its keys, as the help (?) lists them: [["G", "Series"], ...].
    property var help: []
    // Photos shown in the grid as one card, each a list of library
    // positions: the card stands where the first is and shows it, with the
    // mark for several photos. Opening it opens that photo.
    property var stacks: []
    // Photos not shown in the grid at all, library positions: no cell, out
    // of any stack, and passed over by the viewer's ← and → and its strip.
    property var hidden: []
    // Marks in the corner of grid cells, each a PluginMark: the photos
    // whose library field is true, or those it lists, with a switch of
    // their own in the side panel. Four at most.
    property list<QtObject> marks
    // Drawn over a photo shown full window, inside it: zoomed, panned,
    // grown from its cell and clipped with it. Made afresh for each photo
    // shown, with `photo` (its library position) and `shown` (true while
    // that photo is shown, false the moment it changes or the viewer
    // starts closing), which its root declares.
    property Component viewerLayer
    // The kinds of source it brings, each a PluginSource: how Pinhole's
    // side panel, its first run and Add a source show them, and what
    // signing in to one does. Only kinds its manifest's "sources" names.
    property list<QtObject> sources
    // A few steps that show what it does, on the photos and its buttons as
    // they are (Tour.qml): shown by itself the first time it is on, once
    // `tourReady`, and again from the side panel.
    property var tour: []
    property bool tourReady: true
    // While true, a photo shown full window is kept at SDR: on a display
    // that shows more (wide gamut, HDR), what is beyond fades out, and back
    // in once no plugin asks. For something SDR drawn over the photo, as
    // Live Photos' motion is (API 6).
    property bool holdSdr: false
}
