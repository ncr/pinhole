import QtQuick
import Pinhole.Plugin

// Favourites: the heart on F, over the photo shown full window and in
// select mode's bar for what is ticked, filled in the accent while they
// are all favourites; a press makes them all favourites, or none when they
// all are, as in Photos. The library changes at once and the source after
// (pinhole.setFavorite: iCloud's own favourites, a folder's kept in its
// library); what it could not set goes back, and the heart says why. Each
// press is a step to undo. And a mark on favourites in the grid.
PinholePlugin {
    id: plugin

    // Whether these photos (positions) are all favourites, and there are
    // some: by each photo's for a few, as soon as one is not; by the whole
    // column for many (a map made for each of thousands would take a
    // while).
    function allFavourites(photos) {
        if (photos.length === 0)
            return false
        if (photos.length <= 256) {
            for (const photo of photos)
                if (pinhole.library.item(photo).favorite !== true)
                    return false
            return true
        }
        const favourite = pinhole.library.field("favorite")
        return photos.every(i => favourite[i] === true)
    }
    // The ids of these photos that are not `on` already: by the whole
    // columns for many (a map made for each would take a while), by each
    // photo's for a few.
    function idsNot(photos, on) {
        const library = pinhole.library
        if (photos.length <= 256)
            return photos.map(photo => library.item(photo)).filter(it => it.id !== undefined && it.favorite !== on).map(it => it.id)
        const ids = library.field("id"), favourite = library.field("favorite")
        return photos.filter(i => i >= 0 && i < ids.length && favourite[i] !== on).map(i => ids[i])
    }

    // The photo shown full window, and what is ticked: all favourites?
    readonly property bool shownIsFavourite: (pinhole.library.revision, pinhole.viewer.open && pinhole.viewer.index >= 0
                                              && pinhole.library.item(pinhole.viewer.index).favorite === true)
    readonly property bool tickedAreFavourites: (pinhole.library.revision, pinhole.selectionCount > 0
                                                 && allFavourites(pinhole.selection()))

    // All of them favourites, or none when they all are; a step to undo.
    // Undone on the library it was done in only (another's photos may have
    // the same ids; the steps go with it anyway).
    function toggle(photos) {
        if (photos.length === 0)
            return
        const on = !allFavourites(photos)
        const ids = idsNot(photos, on)
        if (ids.length === 0)
            return
        const key = pinhole.library.key
        set(ids, on)
        pinhole.did(on ? qsTr("adding to favourites") : qsTr("removing from favourites"),
                    () => { if (pinhole.library.key === key) set(ids, !on) },
                    () => { if (pinhole.library.key === key) set(ids, on) })
    }
    function set(ids, on) {
        pinhole.setFavorite(ids, on).catch(error => {
            if (error.code !== "cancelled")
                complain(whyNot(error.code))
        })
    }
    // Why not, in Pinhole's words: the source's name from the library's
    // key ("icloud" is iCloud, a folder its name).
    function whyNot(code) {
        const key = pinhole.library.key
        const name = key.split(":")[0] === "icloud" ? "iCloud" : key.substring(key.indexOf(":") + 1).split("/").pop()
        // The window's line to the service, for whether it is running.
        const health = typeof service !== "undefined" && service ? service.health : ""
        if (code === "read_only")
            return qsTr("This library is only being looked at here: its favourites cannot be changed")
        if (code === "needs_sign_in")
            return qsTr("Not signed in to %1").arg(name)
        if (code === "no_service" || health === "no_service")
            return qsTr("The Pinhole service is not running")
        return qsTr("Could not change favourites in %1").arg(name)
    }
    // Under the heart that shows; else (undone in the grid) at the foot of
    // the window.
    function complain(text) {
        if (pinhole.viewer.open)
            shownHeart.complain(text)
        else if (pinhole.selectionCount > 0)
            tickedHeart.complain(text)
        else
            pinhole.notify(text)
    }

    marks: [
        PluginMark {
            name: "favorite"
            text: qsTr("Favourites")
            icon: "heart"
            colour: "red"
            order: 10
            field: "favorite"
        }
    ]

    // A filled heart in the accent, or an empty one (Nerd Font's: the
    // typeface's own ♥ is an outline too). Before Live's, and in select
    // mode before Copy.
    readonly property PluginAction shownAction: shownHeart
    readonly property PluginAction tickedAction: tickedHeart
    actions: [
        PluginAction {
            id: shownHeart
            place: "viewer"
            order: -20
            text: plugin.shownIsFavourite ? "" : ""
            textColour: plugin.shownIsFavourite ? "accent" : ""
            key: "F"
            onTriggered: if (pinhole.viewer.index >= 0) plugin.toggle([pinhole.viewer.index])
        },
        PluginAction {
            id: tickedHeart
            place: "selection"
            order: -20
            text: plugin.tickedAreFavourites ? "" : ""
            textColour: plugin.tickedAreFavourites ? "accent" : ""
            key: "F"
            usable: pinhole.selectionCount > 0
            onTriggered: plugin.toggle(pinhole.selection())
        }
    ]
    help: [["F", qsTr("Favourite")]]
}
