import QtQuick
import Pinhole.Plugin

// app/bin/test-selftests clashes: each key that is Pinhole's own or
// another plugin's in the same place is said under the plugin (PluginPlane's
// findClashes); those that are no one's are not.
PinholePlugin {
    actions: [
        // Pinhole's own on the grid (grid.left).
        PluginAction { place: "grid"; text: "Own key"; key: "Left" },
        // People's, on the grid and over a photo.
        PluginAction { place: "grid"; text: "People's key"; key: "Shift+F" },
        PluginAction { place: "viewer"; text: "Who's key"; key: "W" },
        // No one's.
        PluginAction { place: "grid"; text: "Free key"; key: "Shift+J" },
        PluginAction { place: "viewer"; text: "Free viewer key"; key: "Shift+J" }
    ]
    // A Command of its own: Pinhole's own key, and no one's.
    Command { name: "test.clashes.own"; text: "Own command"; keys: ["Right"]; keyLayer: "grid" }
    Command { name: "test.clashes.free"; text: "Free command"; keys: ["Shift+Q"]; keyLayer: "grid" }
}
