import QtQuick
import Pinhole.Plugin

// iCloud: an Apple ID's photo library as a source. pinholed walks it,
// follows its changes and fetches what is opened (its Rust is this
// plugin's, in source/, linked into pinholed; Apple's sign-in is auth/'s
// pinhole-auth). This is what the window shows of it: its rows in the side
// panel (Sign out on O, "Sign in again" when Apple wants it), what the
// first run and Add a source offer (icloudpd's sign-in, when it is on this
// computer, and signing in), and signing in itself (ICloudSignIn.qml),
// made the first time it is needed.
PinholePlugin {
    id: plugin
    // The sign-in, once made (for the tests).
    readonly property var signInPanel: signIn.item

    // Signing in, over the window: the Apple ID filled in for one added
    // already, else typed.
    function openSignIn(appleId) {
        if (signIn.shown)
            return
        signIn.open(appleId)
    }
    function closeSignIn() { signIn.close() }
    // Whether the source shown is this one, and Apple wants it signed in to
    // again (or it was signed out of).
    function needsSignIn(source) { return source.shown === true && source.health === "needs_sign_in" }

    sources: [
        PluginSource {
            kind: "icloud"
            icon: ""
            noun: "iCloud"
            place: source => "iCloud"
            // icloudpd signed in on this computer: no password needed.
            offers: (pinhole.sources.discovered.icloud?.icloudpd ?? []).length > 0
                    ? [{ title: qsTr("iCloud Photos"),
                         detail: qsTr("icloudpd is signed in on this computer, so no password is needed"),
                         triggered: () => plugin.openSignIn("") }]
                    : []
            ways: [{ title: qsTr("Sign in to iCloud"), key: "I", triggered: () => plugin.openSignIn("") }]
            note: source => plugin.needsSignIn(source) ? qsTr("Sign in again") : ""
            // Signing out keeps its photos here; signed out, a tap on its
            // row signs in again instead.
            actions: source => plugin.needsSignIn(source) ? [] : [{
                text: qsTr("Sign out"),
                key: "O",
                ask: qsTr("Sign out? Its index stays here, so signing in again is quick."),
                triggered: () => pinhole.sources.signOut(source.id).catch(error => {
                    if (error.code !== "cancelled")
                        pinhole.notify(qsTr("Could not sign out: %1").arg(error.message))
                })
            }]
            forgets: source => qsTr("Sign out and forget its index? The photos stay in iCloud.")
            adds: source => qsTr("Uploaded to your iCloud Photos. The files here stay as they are.")
            pushed: qsTr("Apple notifies of changes")
            signIn: source => plugin.openSignIn(source.account ?? "")
        }
    ]

    // Made the first time it opens: not as the plugin loads, before the
    // first frame.
    PluginView { id: signIn; source: "ICloudSignIn.qml" }
}
