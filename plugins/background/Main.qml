import QtQuick
import Pinhole.Plugin

// Background: the photo shown full window as the desktop background, with
// Omarchy's own omarchy-theme-bg-set, which draws a still itself and hands
// a video to OWE. A video is a moving background; a Live Photo's motion
// (as taken, or forward and back, which loops without a jump) is one from
// its ▾ menu.
//
// What is set is kept in Pictures/Pinhole/Backgrounds, not the cache: the
// background is a link to the file, and Omarchy remembers it for the theme
// to bring back after a switch to another theme and back, so it has to
// outlast a cache clean. Each one is a step of Pinhole's history: Ctrl+Z
// puts back the background before it.
PinholePlugin {
    id: plugin

    // Fetching, keeping or setting one: its button's key turns.
    property bool working: false
    readonly property var shown: pinhole.viewer.open ? pinhole.library.item(pinhole.viewer.index) : null

    readonly property string folder: pinhole.picturesDir() + "/Pinhole/Backgrounds"
    // What the shell draws, and what OWE plays (Omarchy's Util.isVideoPath);
    // a photo in another form is made a JPEG, a video in another put in an
    // MKV as it is.
    readonly property var stills: ["jpg", "png", "webp", "gif", "bmp"]
    readonly property var videos: ["mp4", "m4v", "mov", "webm", "mkv", "avi"]
    // Each way of setting one: the service's rendition, and what the kept
    // file's name says after the photo's.
    readonly property var ways: ({
        still: { kind: "full", tag: "" },
        video: { kind: "full", tag: "" },
        live: { kind: "live", tag: " Live" },
        bounce: { kind: "bounce", tag: " Bounce" }
    })

    // Where the photo is kept: named for it and when it was taken, so the
    // same one set again is the same file.
    function keptPath(item, way, from) {
        const dot = from.lastIndexOf(".")
        let extension = dot > from.lastIndexOf("/") ? from.slice(dot + 1).toLowerCase() : ""
        if (extension === "jpeg")
            extension = "jpg"
        if (way === "still")
            extension = stills.includes(extension) && extension !== "jpg" ? extension : "jpg"
        else if (!videos.includes(extension))
            extension = "mkv"
        const name = item.filename || item.id
        const base = name.replace(/\.[^.]*$/, "").replace(/\//g, "-")
        return folder + "/" + base + " " + Qt.formatDateTime(new Date(item.date), "yyyy-MM-dd HH.mm.ss")
            + ways[way].tag + "." + extension
    }

    function setBackground(index, way) {
        if (working)
            return
        const item = pinhole.library.item(index)
        let kept = "", before = ""
        working = true
        pinhole.fetch(index, ways[way].kind)
            // A video's full rendition is its edit as an MP4; one without
            // comes as its streaming rendition.
            .then(from => way === "video" && !videos.includes(from.split(".").pop().toLowerCase())
                  ? pinhole.fetch(index, "video") : from)
            .then(from => {
                kept = keptPath(item, way, from)
                return pinhole.run("keep", [from, kept])
            })
            .then(() => current())
            .then(path => {
                before = path
                return pinhole.run("omarchy-theme-bg-set", [kept])
            })
            .then(() => {
                pinhole.notify(way === "still" ? qsTr("Your desktop background now")
                                               : qsTr("Your moving desktop background now"))
                if (before !== "" && before !== kept)
                    pinhole.did(qsTr("setting the background"), () => apply(before), () => apply(kept))
            })
            .catch(failed)
            .then(() => working = false)
    }
    function failed(error) {
        if (error.code === "cancelled")
            return
        const why = String(error.message || error.code || error).trim().split("\n").pop()
        pinhole.notify(qsTr("Could not set the background: %1").arg(why))
    }
    // The background set now, as a path ("" if none): the shell's latest
    // choice, which it points the link at once nothing on screen can flash,
    // else the link.
    function current() {
        return pinhole.run("sh", ["-c", 'chosen=$(omarchy-shell shell selectedBackground 2>/dev/null) && [ -f "$chosen" ] && echo "$chosen" '
                                        + '|| readlink -f "$HOME/.local/state/omarchy/current/background"'])
            .then(result => result.stdout.trim(), () => "")
    }
    // Undone or done again: the one before, or this one, if it is still there.
    function apply(path) {
        pinhole.run("omarchy-theme-bg-set", [path]).catch(failed)
    }

    actions: [
        PluginAction {
            id: stillAction
            place: "viewer"
            group: "background"
            text: qsTr("Background")
            key: "B"
            shown: plugin.shown !== null && !plugin.shown.video
            busy: plugin.working
            onTriggered: plugin.setBackground(pinhole.viewer.index, "still")
        },
        PluginAction {
            place: "viewer"
            group: "background"
            text: qsTr("Background")
            key: "B"
            shown: plugin.shown !== null && plugin.shown.video
            busy: plugin.working
            onTriggered: plugin.setBackground(pinhole.viewer.index, "video")
        },
        PluginAction {
            place: "viewer"
            group: "background"
            text: qsTr("Moving background")
            shown: plugin.shown !== null && plugin.shown.live
            usable: !plugin.working
            onTriggered: plugin.setBackground(pinhole.viewer.index, "live")
        },
        PluginAction {
            place: "viewer"
            group: "background"
            text: qsTr("Bouncing background")
            shown: plugin.shown !== null && plugin.shown.live
            usable: !plugin.working
            onTriggered: plugin.setBackground(pinhole.viewer.index, "bounce")
        }
    ]

    help: [
        ["B", qsTr("Make the photo or video the desktop background")],
        ["⇧B", qsTr("A Live Photo's motion as a moving background")]
    ]

    tour: [
        {
            title: qsTr("Behind everything"),
            text: qsTr("On a photo shown full window, B makes it the desktop background; a video becomes a moving one. Ctrl+Z puts the one before back."),
            open: () => pinhole.currentIndex(),
            action: stillAction
        },
        {
            title: qsTr("Live Photos move"),
            text: qsTr("On a Live Photo, the ▾ beside Background (⇧B) makes its motion the background, as taken or bouncing forward and back."),
            action: stillAction
        },
        {
            title: qsTr("Kept"),
            text: qsTr("Each one is kept in Pictures/Pinhole/Backgrounds, so it stays when the cache is cleaned and comes back with its theme."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 8
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.foreground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: qsTr("B on a photo shown full window. Each background is kept in Pictures/Pinhole/Backgrounds.")
            }
            ShellButton {
                text: qsTr("Show the folder")
                visible: pinhole.exists(plugin.folder)
                onTapped: pinhole.showFile(plugin.folder)
            }
        }
    }
}
