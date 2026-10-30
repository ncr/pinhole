import QtQuick

// Signing in to an Apple ID, in the window: the Apple ID and password (or
// none, when icloudpd is signed in to that Apple ID here), then the code
// Apple sends to the person's devices, in six boxes that go on by
// themselves after the sixth digit. Signed in, the Apple ID is added and
// its photos shown.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false
    // Filled in already: signing in again to an Apple ID added before.
    property string appleId

    readonly property bool busy: sources.signInState === "busy"
    readonly property bool needsCode: sources.signInState === "needs_code"
    // Past the password: the code is what is asked for.
    property bool codeStage: false
    readonly property bool fromIcloudpd: !codeStage && sources.icloudpdHas(account.text)
    readonly property bool ready: account.text.indexOf("@") > 0 && (fromIcloudpd || password.text !== "")

    onNeedsCodeChanged: if (needsCode) {
        codeStage = true
        code.text = ""
        code.forceActiveFocus()
    }
    onShownChanged: {
        if (shown) {
            sources.cancelSignIn()
            codeStage = false
            account.text = appleId
            password.text = ""
            code.text = ""
            Qt.callLater(() => (appleId === "" ? account.input : password.input).forceActiveFocus())
        } else {
            sources.cancelSignIn()
            appleId = ""
        }
    }
    Connections {
        target: sources
        function onSignedIn() { root.shown = false }
    }

    function submit() {
        if (busy)
            return
        if (codeStage) {
            if (code.text.length === 6)
                sources.submitCode(account.text, code.text)
        } else if (ready) {
            sources.signIn(account.text, fromIcloudpd ? "" : password.text)
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
        keyLayer: "signIn"
        onTriggered: root.submit()
    }
    Command {
        id: closeAction
        name: "signIn.close"
        keys: ["Escape"]
        keyLayer: "signIn"
        onTriggered: root.shown = false
    }
    Command {
        id: newCodeAction
        name: "signIn.newCode"
        keys: ["Ctrl+R"]
        keyLayer: "signIn"
        when: root.codeStage && password.text !== "" && !root.busy
        onTriggered: {
            code.text = ""
            sources.signIn(account.text, password.text)
            code.forceActiveFocus()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
    }
    MouseArea {
        anchors.fill: parent
        onClicked: if (!root.busy) root.shown = false
    }

    Rectangle {
        id: card
        readonly property int padding: 2 * theme.fontSize
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, theme.fontSize * 36)
        height: Math.min(parent.height - 32, content.implicitHeight + 2 * padding)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        clip: true
        MouseArea { anchors.fill: parent }

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
                visible: sources.signInError !== "" && !root.busy
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.red
                text: sources.signInError
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
            }
            Item {
                width: parent.width
                height: go.height
                UiText {
                    width: parent.width - go.width - theme.fontSize
                    y: centredIn(parent.height)
                    wrapMode: Text.WordWrap
                    color: theme.darkForeground
                    text: qsTr("Pinhole keeps the session, never the password.")
                }
                ShellButton {
                    id: go
                    anchors.right: parent.right
                    visible: !root.codeStage
                    active: root.ready && !root.busy
                    usable: root.ready && !root.busy
                    text: root.fromIcloudpd ? qsTr("Use icloudpd's sign-in") : qsTr("Sign in")
                    action: submitAction
                }
                ShellButton {
                    anchors.right: parent.right
                    visible: root.codeStage && password.text !== ""
                    usable: !root.busy
                    text: qsTr("Send a new code")
                    action: newCodeAction
                }
            }
        }
    }
}
