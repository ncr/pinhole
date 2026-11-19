import QtQuick

// Text as Omarchy sets it: its typeface, size and the theme's text colour.
// Plain text: what it shows may be a photo's, a source's or a plugin's
// words, never markup to read; one that styles its own text says
// Text.StyledText.
Text {
    id: uiText
    textFormat: Text.PlainText
    font.family: theme.font
    font.pixelSize: theme.fontSize
    color: theme.foreground

    // Where to put this text in a box `height` tall so its letters sit in
    // the middle: centred on the capital height, as native toolkits align
    // labels, rather than on the line box, whose room for descenders left
    // the text riding high. (QML's FontMetrics has no capHeight; a capital's
    // tight bounds give it.)
    function centredIn(height) {
        if (lineCount > 1)
            return Math.round((height - implicitHeight) / 2)
        const cap = metrics.tightBoundingRect("H").height
        return Math.round((height + cap) / 2 - metrics.ascent)
    }
    FontMetrics {
        id: metrics
        font: uiText.font
    }
}
