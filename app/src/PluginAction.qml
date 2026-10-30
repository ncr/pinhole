import QtQml
import QtQuick

// A button a plugin adds, drawn as Pinhole's own (docs/plugins.md).
QtObject {
    // "grid": at the top right of the grid, before Select. "viewer": in the
    // row at the top right of a photo shown full window, before Copy.
    // "selection": in select mode's bar, after Export, for what is ticked
    // (pinhole.selection()); its key works only while it shows.
    property string place: "grid"
    // In the bars (the viewer's, select mode's): actions with the same
    // group are one button, the first of them shown, and the rest in its
    // menu under ▾, opened with ⇧ and its key too. Without one, alone.
    property string group
    property string text
    // A key that presses it while it shows: "G", "Shift+G".
    property string key
    property bool shown: true
    property bool usable: true
    // Lit, in the accent.
    property bool active: false
    // On its way: an arc turns in its key's place.
    property bool busy: false
    // The pointer over its button, for a plugin to show what pressing it
    // would do: Pinhole sets it.
    property bool hovered: false
    // Its button, where Pinhole draws it, for a tour step to light it.
    property Item button: null
    signal triggered()
}
