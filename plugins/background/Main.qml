import QtQuick
import Pinhole.Plugin

// Background: the photo shown full window as the desktop background, with
// Omarchy's own omarchy-theme-bg-set, which draws a still itself and hands
// a video to OWE. A video is a moving background; a Live Photo's motion
// (as taken, or its Loop or Bounce, which loop without a jump) is one from
// its ▾ menu.
//
// What is set is kept in Pictures/Pinhole/.backgrounds, not the cache: the
// background is a link to the file, and Omarchy remembers it for the theme
// to bring back after a switch to another theme and back, so it has to
// outlast a cache clean. Hidden, as a folder source of Pictures would show
// each one beside its photo. Each is a step of Pinhole's history: Ctrl Z
// puts back the background before it. The folder is pruned as one is kept,
// of what `background keep` wrote there alone: the newest 12 stay, and any
// in use or a theme will bring back; a file put there otherwise stays, and
// a .backgrounds that is a link is not used at all.
PinholePlugin {
    id: plugin

    // Fetching, keeping or setting one: its button's key turns.
    property bool working: false
    readonly property var shown: pinhole.viewer.open ? pinhole.library.item(pinhole.viewer.index) : null

    readonly property string folder: pinhole.picturesDir() + "/Pinhole/.backgrounds"
    // What the shell draws, and what OWE plays (Omarchy's Util.isVideoPath);
    // a photo in another form is made a JPEG, a video in another put in an
    // MKV as it is.
    readonly property var stills: ["jpg", "png", "webp", "gif", "bmp"]
    readonly property var videos: ["mp4", "m4v", "mov", "webm", "mkv", "avi"]
    // Each way of setting one: the service's rendition, and what the kept
    // file's name says after the photo's. A Live Photo's `loop` is the Loop
    // set on the iPhone, else its motion as taken.
    readonly property var ways: ({
        still: { kind: "full", tag: "" },
        video: { kind: "full", tag: "" },
        live: { kind: "loop", tag: " Live" },
        bounce: { kind: "bounce", tag: " Bounce" }
    })

    function extensionOf(path) {
        const name = path.split("/").pop()
        const dot = name.lastIndexOf(".")
        return dot > 0 ? name.slice(dot + 1).toLowerCase() : ""
    }
    function stemOf(path) { return path.split("/").pop().replace(/\.[^.]*$/, "").toLowerCase() }

    function setBackground(index, way) {
        if (working)
            return
        const item = pinhole.library.item(index)
        let kept = "", before = null
        working = true
        pinhole.fetch(index, ways[way].kind)
            // A video's full rendition is its edit as an MP4, but some have
            // only a still there: then the video as taken, at its own size.
            .then(from => way === "video" && !videos.includes(extensionOf(from)) ? pinhole.fetch(index, "original") : from)
            .then(from => {
                let extension = extensionOf(from)
                if (way === "still")
                    extension = stills.includes(extension) && extension !== "jpg" ? extension : "jpg"
                else if (!videos.includes(extension))
                    extension = "mkv"
                // Named for the photo and when it was taken; `background
                // keep` adds a few characters of the rendition. A folder's
                // own file is the photo's name; the cache's are its keys.
                const name = (item.filename || item.id).replace(/\.[^.]*$/, "").replace(/\//g, "-")
                    + " " + Qt.formatDateTime(new Date(item.date), "yyyy-MM-dd HH.mm.ss") + ways[way].tag
                const own = stemOf(from) === stemOf(item.filename || "")
                return pinhole.run("background", ["keep", from, folder, name, extension, own ? "own" : "cached"])
            })
            .then(result => {
                kept = result.stdout.trim()
                return current()
            })
            .then(now => {
                before = now
                return pinhole.run("background", ["set", kept, ""])
            })
            .then(() => {
                pinhole.notify(way === "still" ? qsTr("Your desktop background now")
                                               : qsTr("Your moving desktop background now"))
                if (before.path !== "" && before.path !== kept)
                    pinhole.did(qsTr("setting the background"), () => apply(before), () => apply({ path: kept, theme: before.theme }))
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
    // The background set now, and the theme it is set under.
    function current() {
        return pinhole.run("background", ["current"]).then(result => {
            const lines = result.stdout.split("\n")
            return { theme: lines[0].trim(), path: (lines[1] || "").trim() }
        }, () => ({ theme: "", path: "" }))
    }
    // Undone or done again: that one, if it is still there.
    function apply(background) {
        pinhole.run("background", ["set", background.path, background.theme]).catch(error => {
            if (error.exitCode === 3)
                pinhole.notify(qsTr("The background before went with the theme %1").arg(background.theme))
            else if (error.exitCode === 4)
                pinhole.notify(qsTr("The background before is gone: %1").arg(background.path.split("/").pop()))
            else
                failed(error)
        })
    }
    // A Live Photo for the tour to show the ▾ on: the newest, found once.
    property string touringId: ""
    function aLivePhoto() {
        if (touringId === "") {
            touringId = "-"
            const live = pinhole.library.field("live")
            for (let i = live.length - 1; i >= 0; --i)
                if (live[i]) {
                    touringId = pinhole.library.item(i).id
                    break
                }
        }
        const index = pinhole.library.indexOf(touringId)
        return index >= 0 ? index : nearestPhoto(pinhole.currentIndex())
    }
    // The photo nearest `index` that is not a video: the still's
    // Background, which the tour lights, shows on one.
    function nearestPhoto(index) {
        const videos = pinhole.library.field("video")
        for (let d = 0; d < videos.length; ++d)
            for (const i of [index - d, index + d])
                if (i >= 0 && i < videos.length && !videos[i])
                    return i
        return index
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
            key: "V"
            shown: plugin.shown !== null && plugin.shown.live
            usable: !plugin.working
            onTriggered: plugin.setBackground(pinhole.viewer.index, "live")
        },
        PluginAction {
            place: "viewer"
            group: "background"
            text: qsTr("Bounce as background")
            key: "Shift+V"
            shown: plugin.shown !== null && plugin.shown.live
            usable: !plugin.working
            onTriggered: plugin.setBackground(pinhole.viewer.index, "bounce")
        }
    ]

    help: [
        ["B", qsTr("Make it the desktop background")],
        ["V", qsTr("A Live Photo's motion")],
        ["⇧ V", qsTr("A Live Photo's Bounce")]
    ]

    tour: [
        {
            title: qsTr("Behind everything"),
            text: qsTr("On a photo shown full window, B makes it the desktop background; a video becomes a moving one. Ctrl\u00a0Z puts the one before back."),
            open: () => plugin.nearestPhoto(pinhole.currentIndex()),
            action: stillAction
        },
        {
            title: qsTr("Live Photos move"),
            text: qsTr("On a Live Photo, V (or the ▾ beside Background, ⇧\u00a0B) makes its motion the background: as taken, or as its Loop if the iPhone set one; ⇧\u00a0V as a Bounce."),
            open: () => aLivePhoto(),
            action: stillAction
        },
        {
            title: qsTr("Kept"),
            text: qsTr("Each one is kept in Pictures/Pinhole/.backgrounds, hidden from the grid, so it stays when the cache is cleaned and comes back with its theme."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            id: settingsColumn
            width: parent ? parent.width : 0
            spacing: 8
            // What the folder holds, read as the settings show.
            property string held: ""
            Component.onCompleted: pinhole.run("background", ["size", plugin.folder]).then(result => {
                const counted = result.stdout.trim().split(" ").map(Number)
                held = counted[0] > 0 ? qsTr("%n kept there now, %1.", "", counted[0]).arg(pinhole.sizeText(counted[1])) : ""
            }, () => {})
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.foreground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: qsTr("B on a photo shown full window. Each background is kept in Pictures/Pinhole/.backgrounds: the newest 12, and any in use or a theme will bring back.")
                      + (settingsColumn.held !== "" ? " " + settingsColumn.held : "")
            }
            ShellButton {
                text: qsTr("Show the folder")
                visible: pinhole.exists(plugin.folder)
                // The folder itself (showFile opens a file's folder).
                onTapped: pinhole.showFile(plugin.folder + "/")
            }
        }
    }
}
