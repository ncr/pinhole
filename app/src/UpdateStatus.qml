import QtQuick

// Shared words for the small notice and the side panel. Checking quietly
// never takes focus, opens a dialog, or interrupts browsing.
QtObject {
    readonly property bool busy: updater.state === "checking" || updater.state === "updating"
    readonly property string text: {
        switch (updater.state) {
        case "checking": return qsTr("Checking for updates…")
        case "available": return qsTr("A new Pinhole is available")
        case "updating": return qsTr("Updating… You can keep browsing")
        case "ready": return qsTr("Updated · restart when you’re ready")
        case "current": return qsTr("Pinhole is up to date")
        case "unavailable": return qsTr("Updates are managed by your installer")
        case "error":
            switch (updater.error) {
            case "dirty": return qsTr("Save and commit your changes before updating")
            case "branch": return qsTr("Switch the development checkout to main to update")
            case "diverged": return qsTr("Your main has local commits · update it in the repository")
            case "package": return qsTr("Update this package with bin/install in the repository")
            case "network": return qsTr("Couldn’t check for updates · try again when online")
            case "changed": return qsTr("A newer update arrived · check again")
            case "build": return qsTr("The update couldn’t be built · your running version is still open")
            case "busy": return qsTr("An update is already running")
            case "restart": return qsTr("Close Pinhole and open it again to finish updating")
            default: return qsTr("Couldn’t update Pinhole · try again")
            }
        default: return qsTr("Updates from main · checked in the background")
        }
    }
    readonly property string button: updater.state === "ready" ? qsTr("Restart")
                                            : updater.state === "available" ? qsTr("Update") : qsTr("Check for updates")
    function press() {
        if (updater.state === "ready") updater.restart()
        else if (updater.state === "available") updater.apply()
        else updater.check()
    }
}
