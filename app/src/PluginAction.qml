import QtQml
import QtQuick

// A button a plugin adds, drawn as Pinhole's own (docs/plugins.md).
QtObject {
    // "grid": at the top right of the grid, before Select. "viewer": in the
    // row at the top right of a photo shown full window, before Copy.
    // "selection": in select mode's bar, after Copy, for what is ticked
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
    // Its words' colour on the button when not lit: a theme colour's name
    // (red, green, yellow, blue, magenta, cyan, accent) or "#rrggbb"; ""
    // the theme's text. A favourite's heart is "accent".
    property string textColour
    // On its way: an arc turns in its key's place, once it has been for
    // `busyDelay` ms (soon, for what is mostly at once: Live's 300).
    property bool busy: false
    property int busyDelay: 1000
    // The pointer over its button, for a plugin to show what pressing it
    // would do: Pinhole sets it.
    property bool hovered: false
    // Its button, where Pinhole draws it, for a tour step to light it.
    property Item button: null
    // Where it stands among the actions of its place: lowest first, then
    // in the plugins' order. In select mode's bar, below 0 is before
    // Pinhole's Copy, 0 and up after it.
    property int order: 0
    // In its group's menu, a word at the right of its row in place of its
    // key: "iPhone".
    property string hint
    signal triggered()
    // What it could not do, in a few words: said under its button (under
    // More ▾ when folded there) for a few seconds, the button shaking.
    signal complained(string text)
    function complain(text) { complained(text) }
}
