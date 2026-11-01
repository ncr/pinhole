import QtQuick
import Pinhole.Plugin

// Signing in to an Apple ID, over the window: the Apple ID and password (or
// none, when icloudpd is signed in to that Apple ID here), then the code
// Apple sends to the person's devices, in six boxes that go on by
// themselves after the sixth digit. Signed in, the Apple ID is added and
// its photos shown (pinhole.sources.signIn, submitCode).
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false
    // Filled in already: signing in again to an Apple ID added before.
    property string appleId
    function open(id) {
        appleId = id
        shown = true
    }

    // "" (nothing under way), "busy" (Apple is asked), "needs_code"; and
    // what went wrong, said for people.
    property string stage: ""
    property string error: ""
    // Each request made; an answer to one before the last is not waited
    // for (closed meanwhile, or asked again).
    property int asked: 0
    readonly property bool busy: stage === "busy"
    readonly property bool needsCode: stage === "needs_code"
    // Past the password: the code is what is asked for.
    property bool codeStage: false
    readonly property bool fromIcloudpd: !codeStage && icloudpdHas(account.text)
    readonly property bool ready: account.text.indexOf("@") > 0 && (fromIcloudpd || password.text !== "")

    // Whether icloudpd is signed in to this Apple ID here, so no password is
    // needed: its session files are named after the Apple ID with only
    // letters, digits and _ kept (discover's icloudpd).
    function icloudpdHas(id) {
        const stem = id.trim().replace(/[^A-Za-z0-9_]/g, "")
        const known = pinhole.sources.discovered.icloud?.icloudpd ?? []
        return stem !== "" && id.indexOf("@") >= 0 && known.indexOf(stem) >= 0
    }
    // What each of the sign-in provider's codes means for the person
    // signing in.
    function said(code) {
        switch (code) {
        case "": return ""
        case "wrong_password": return qsTr("Apple did not accept that Apple ID or password.")
        case "wrong_code": return qsTr("That code did not work. The newest one Apple sent is the one to type.")
        case "try_again": return qsTr("Apple's sign-in did not go through this time. Try again in a moment.")
        case "rate_limited": return qsTr("Apple is not taking sign-ins for this Apple ID right now. Try once more in half an hour.")
        case "needs_sign_in": return qsTr("icloudpd's sign-in for this Apple ID has run out. Sign in with the password instead.")
        case "unavailable": return qsTr("iCloud cannot be reached. Is this computer online?")
        case "read_only": return qsTr("This library is only being looked at here: sign in from a window of your own library.")
        case "turned_off": return qsTr("iCloud is turned off: turn it on in the side panel (Tab), under Plugins.")
        }
        return qsTr("Signing in did not work (%1).").arg(code)
    }
    function settle(stage, code) {
        root.stage = stage
        root.error = said(code || "")
    }
    function cancel() {
        ++asked
        settle("")
    }
    function signIn(id, secret) {
        // A code asked for again keeps the code boxes up.
        const back = stage === "needs_code" ? stage : ""
        settle("busy")
        const mine = ++asked
        const params = { apple_id: id.trim() }
        if (secret !== "")
            params.password = secret
        pinhole.sources.signIn("icloud", params).then(result => {
            if (mine !== asked)
                return
            if (result.state === "needs_code") {
                settle("needs_code")
            } else {
                settle("")
                root.shown = false
            }
        }, error => {
            if (mine === asked && error.code !== "cancelled")
                settle(back, error.code)
        })
    }
    function submitCode(id, digits) {
        settle("busy")
        const mine = ++asked
        pinhole.sources.submitCode("icloud", { apple_id: id.trim(), code: digits }).then(() => {
            if (mine !== asked)
                return
            settle("")
            root.shown = false
        }, error => {
            if (mine === asked && error.code !== "cancelled")
                settle("needs_code", error.code)
        })
    }

    onNeedsCodeChanged: if (needsCode) {
        codeStage = true
        code.text = ""
        if (shown)
            code.forceActiveFocus()
    }
    onShownChanged: {
        if (shown) {
            cancel()
            // Whether icloudpd is signed in here, afresh.
            pinhole.sources.discover()
            codeStage = false
            account.text = appleId
            password.text = ""
            code.text = ""
            Qt.callLater(() => {
                if (shown)
                    (appleId === "" ? account.input : password.input).forceActiveFocus()
            })
        } else {
            cancel()
            appleId = ""
            // Its fields let go of the keys: one hidden with them would
            // take Pinhole's (each a shortcut a field takes first).
            for (const field of [account.input, password.input, code])
                field.focus = false
        }
    }

    function submit() {
        if (busy)
            return
        if (codeStage) {
            if (code.text.length === 6)
                submitCode(account.text, code.text)
        } else if (ready) {
            signIn(account.text, fromIcloudpd ? "" : password.text)
        } else if (account.input.activeFocus && account.text !== "") {
            // Enter after the Apple ID: on to the password.
            password.input.forceActiveFocus()
        }
    }

    // Its keys, while it is on top (Command.qml).
    Command {
        id: submitAction
        name: "signIn.submit"
        keys: ["Return", "Enter"]
        keysText: "⏎"
        keyLayer: "plugin"
        when: root.shown
        onTriggered: root.submit()
    }
    Command {
        id: closeAction
        name: "signIn.close"
        keys: ["Escape"]
        keyLayer: "plugin"
        when: root.shown
        onTriggered: root.shown = false
    }
    // Tab and ⇧ Tab between the Apple ID and the password, as anywhere.
    function nextField(direction) {
        const fields = [account.input, password.input].filter(f => f.visible && f.enabled)
        if (fields.length === 0)
            return
        const at = fields.findIndex(f => f.activeFocus)
        fields[(at + direction + fields.length) % fields.length].forceActiveFocus()
    }
    Command {
        name: "signIn.next"
        keys: ["Tab"]
        keyLayer: "plugin"
        when: root.shown && !root.codeStage
        mouse: qsTr("Click the field")
        touchpad: qsTr("Tap the field")
        onTriggered: root.nextField(1)
    }
    Command {
        name: "signIn.previous"
        keys: ["Shift+Tab", "Backtab"]
        keyLayer: "plugin"
        when: root.shown && !root.codeStage
        mouse: qsTr("Click the field")
        touchpad: qsTr("Tap the field")
        onTriggered: root.nextField(-1)
    }
    Command {
        name: "signIn.type"
        noMouse: qsTr("Typing an Apple ID, a password or a code needs a keyboard")
        noTouchpad: qsTr("Typing an Apple ID, a password or a code needs a keyboard")
        noKeyboard: qsTr("Typed")
        keyLayer: "plugin"
        bound: false
    }
    Command {
        id: newCodeAction
        name: "signIn.newCode"
        keys: ["Ctrl+R"]
        keyLayer: "plugin"
        when: root.shown && root.codeStage && password.text !== "" && !root.busy
        onTriggered: {
            code.text = ""
            root.signIn(account.text, password.text)
            code.forceActiveFocus()
        }
    }

    // A click beside it puts it away; nothing there reaches what is under
    // it (the photos, the side panel): no button, no wheel.
    Panel {
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
        onClicked: if (!root.busy) root.shown = false
    }

    Card {
        id: card
        objectName: "signInCard"
        readonly property int padding: 2 * theme.fontSize
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, theme.fontSize * 36)
        height: Math.min(parent.height - 32, content.implicitHeight + 2 * padding)
        radius: Math.min(4, theme.radius)
        border.color: theme.foreground
        clip: true

        Column {
            id: content
            anchors.fill: parent
            anchors.margins: card.padding
            spacing: Math.round(theme.fontSize * 1.286)

            Item {
                width: parent.width
                height: close.height
                UiText {
                    y: centredIn(parent.height)
                    text: qsTr("Sign in to iCloud")
                    color: theme.accent
                }
                ShellButton {
                    id: close
                    anchors.right: parent.right
                    flat: true
                    text: "✕"
                    action: closeAction
                }
            }

            Column {
                width: parent.width
                spacing: 8
                FieldInput {
                    id: account
                    placeholder: qsTr("Apple ID")
                    done: root.codeStage
                    input.inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                    onAccepted: root.submit()
                }
                FieldInput {
                    id: password
                    visible: !root.fromIcloudpd
                    placeholder: qsTr("Password")
                    password: true
                    done: root.codeStage
                    onAccepted: root.submit()
                }
                UiText {
                    visible: root.fromIcloudpd
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: theme.accent
                    text: "✓ " + qsTr("icloudpd is signed in to this Apple ID here, so no password is needed.")
                }
            }

            // The code, once Apple asks for it.
            Column {
                visible: root.codeStage
                width: parent.width
                spacing: theme.fontSize
                UiText {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Apple sent a code to your iPhone and other devices.")
                    color: theme.brightForeground
                }
                Item {
                    width: parent.width
                    height: boxes.height
                    // Typed and pasted into, unseen: the boxes show it.
                    TextInput {
                        id: code
                        width: 1
                        height: 1
                        opacity: 0
                        inputMethodHints: Qt.ImhDigitsOnly
                        onTextChanged: {
                            const digits = text.replace(/\D/g, "").slice(0, 6)
                            if (digits !== text)
                                text = digits
                            else if (digits.length === 6)
                                root.submit()
                        }
                    }
                    Row {
                        id: boxes
                        spacing: 8
                        Repeater {
                            model: 6
                            delegate: Rectangle {
                                required property int index
                                readonly property bool next: code.activeFocus && index === Math.min(code.text.length, 5)
                                width: Math.round(theme.fontSize * 3.7)
                                height: Math.round(theme.fontSize * 4.6)
                                radius: Math.min(4, theme.radius)
                                color: "transparent"
                                border.width: 1
                                border.color: next ? theme.accent : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
                                Rectangle {
                                    visible: parent.next
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: 3
                                    color: theme.accent
                                }
                                UiText {
                                    anchors.centerIn: parent
                                    text: code.text.charAt(parent.index)
                                    color: theme.brightForeground
                                    font.pixelSize: Math.round(theme.fontSize * 2)
                                }
                            }
                        }
                    }
                    MouseArea {
                        anchors.fill: boxes
                        cursorShape: Qt.IBeamCursor
                        onClicked: code.forceActiveFocus()
                    }
                }
                UiText {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: theme.darkForeground
                    text: qsTr("Pasting it works too. It goes on by itself after the sixth digit.")
                }
            }

            Row {
                visible: root.busy
                spacing: theme.fontSize
                Spinner { busy: root.busy; delay: 0 }
                UiText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Asking Apple…")
                }
            }

            UiText {
                visible: root.error !== "" && !root.busy
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.red
                text: root.error
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
            }
            Item {
                id: footer
                // Beside whichever button shows: Sign in, or at the code,
                // Send a new code (none while no password was typed).
                readonly property Item button: go.visible ? go : newCode.visible ? newCode : null
                width: parent.width
                height: Math.max(go.height, footnote.implicitHeight)
                UiText {
                    id: footnote
                    width: parent.width - (footer.button ? footer.button.width + theme.fontSize : 0)
                    y: centredIn(parent.height)
                    wrapMode: Text.WordWrap
                    color: theme.darkForeground
                    text: qsTr("Pinhole keeps the session, never the password.")
                }
                ShellButton {
                    id: go
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !root.codeStage
                    active: root.ready && !root.busy
                    usable: root.ready && !root.busy
                    text: root.fromIcloudpd ? qsTr("Use icloudpd's sign-in") : qsTr("Sign in")
                    action: submitAction
                }
                ShellButton {
                    id: newCode
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.codeStage && password.text !== ""
                    usable: !root.busy
                    text: qsTr("Send a new code")
                    action: newCodeAction
                }
            }
        }
    }
}
