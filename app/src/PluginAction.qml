import QtQml

// A button a plugin adds, drawn as Pinhole's own (docs/plugins.md).
QtObject {
    // "grid": at the top right of the grid, before Select. "viewer": in the
    // row at the top right of a photo shown full window, before Copy.
    // "selection": in select mode's bar, after Export, for what is ticked
    // (pinhole.selection()); its key works only while it shows.
    property string place: "grid"
    property string text
    // A key that presses it while it shows: "G", "Shift+G".
    property string key
    property bool shown: true
    property bool usable: true
    // Lit, in the accent.
    property bool active: false
    // On its way: an arc turns in its key's place.
    property bool busy: false
    signal triggered()
}
