import QtQml
import "../Details.js" as Details
import "../Places.js" as Places

// What check runs: each file's facts as the panel has them, against what
// it should be.
QtObject {
    function text(path) {
        const request = new XMLHttpRequest()
        request.open("GET", "file://" + path, false)
        request.send()
        return request.responseText
    }
    Component.onCompleted: {
        const args = Qt.application.arguments
        const work = args[args.length - 2], count = Number(args[args.length - 1])
        let wrong = 0
        const expect = (what, got, want) => {
            if (got !== want) {
                console.log("WRONG " + what + ": " + got + ", not " + want)
                ++wrong
            }
        }
        // The numbers the panel writes.
        expect("1/46 s", Details.shutterText(1 / 46), "1/46 s")
        expect("1/4 s", Details.shutterText(0.25), "1/4 s")
        expect("0.8 s", Details.shutterText(0.8), "0.8 s")
        expect("0.4 s", Details.shutterText(0.4), "0.4 s")
        expect("2 s", Details.shutterText(2), "2 s")
        expect("59.94 fps", Details.fpsText(60000 / 1001), "60 fps")
        expect("clock", Details.clock(65.4), "1:05")
        // In the language's way.
        const polish = Qt.locale("pl_PL"), english = Qt.locale("en_GB")
        expect("0.8 s in Polish", Details.shutterText(0.8, polish), "0,8 s")
        expect("24.5 MP in Polish", Details.decimal(24.48, 1, polish), "24,5")
        expect("6.8 mm in English", Details.decimal(6.765, 1, english), "6.8")
        expect("12.5 fps in Polish", Details.fpsText(12.5, polish), "12,5 fps")
        const place = { latitude: 49.964712, longitude: -18.5 }
        expect("where, in Polish", Details.coordinates(place, polish), "49,96471° N; 18,5° W")
        expect("where, in English", Details.coordinates(place, english), "49.96471° N, 18.5° W")
        const west = Places.point("west", 0, -179.99, 100)
        const east = Places.point("east", 0, 179.99, 200)
        const view = Places.fit([west, east], 500, 400)
        expect("near the date line", Math.abs(Places.delta(view.x, 0)) < 0.00001, true)
        expect("merged on the overview", Places.clusters([west, east], view.x, view.y, 8, 500, 400).length, 1)
        expect("separate on zoom", Places.clusters([west, east], view.x, view.y, 14, 500, 400).length, 2)
        expect("invalid coordinates", Places.point("bad", 91, 0, 0), null)

        for (let n = 1; n <= count; ++n) {
            const want = text(work + "/" + n + ".want").split("\n")
            const facts = Details.fromOriginal(text(work + "/" + n + ".out"))
            console.log(want[0] + "\n  " + JSON.stringify(facts))
            if (want[1])
                expect(want[0] + " size", facts.width + " " + facts.height, want[1])
        }
        console.log(wrong === 0 ? "All right." : wrong + " wrong.")
        Qt.exit(wrong === 0 ? 0 : 1)
    }
}
