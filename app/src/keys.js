.pragma library

// A key sequence as the buttons and the help show it, in Omarchy's way:
// "Escape" as Esc, Shift as ⇧, "Ctrl+Shift+E" as "Ctrl ⇧ E".
function nameOf(sequence) {
    return sequence.replace("Escape", "Esc").replace(/Shift\+/g, "⇧ ").replace(/Ctrl\+/g, "Ctrl ")
}
