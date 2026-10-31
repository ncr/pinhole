.pragma library

// What pinhole-info reads, as plain facts for the panel (Main.qml): no
// words to translate here, only numbers and the camera's own names.

// The index's row (pinhole-info index): what is known before the original
// is here.
function fromIndex(row) {
    const facts = {}
    if (!row)
        return facts
    if (row.size > 0)
        facts.bytes = row.size
    if (row.width > 0 && row.height > 0) {
        // Turned on its side (EXIF orientations 5 to 8): as it is shown.
        const turned = row.orientation >= 5 && row.orientation <= 8
        facts.width = turned ? row.height : row.width
        facts.height = turned ? row.width : row.height
    }
    facts.format = formatOf(row.fileType || row.type || "")
    facts.hasLocation = row.location === 1
    if (row.duration > 0)
        facts.duration = row.duration
    return facts
}

// A uniform type identifier (iCloud's) or a file name's extension, as a
// format's short name.
function formatOf(type) {
    const known = {
        "public.heic": "HEIC", "public.heif": "HEIF", "public.jpeg": "JPEG", "public.png": "PNG",
        "public.tiff": "TIFF", "com.compuserve.gif": "GIF", "org.webmproject.webp": "WebP", "public.avif": "AVIF",
        "com.adobe.raw-image": "DNG", "com.apple.quicktime-movie": "MOV", "public.mpeg-4": "MP4",
        "com.apple.m4v-video": "M4V", "public.avi": "AVI"
    }
    if (known[type])
        return known[type]
    if (/raw-image$/.test(type))
        return "RAW"
    const dot = type.lastIndexOf(".")
    return dot >= 0 ? type.slice(dot + 1).toUpperCase() : type.toUpperCase()
}

const raw = ["DNG", "CR2", "CR3", "NEF", "NRW", "ARW", "SR2", "RAF", "ORF", "RW2", "PEF", "SRW", "RWL", "3FR", "IIQ", "X3F", "RAW"]
function isRaw(format) { return raw.indexOf(format) >= 0 }

// What pinhole-info read prints: "image" and ImageMagick's lines, or
// "video" and ffprobe's JSON.
function fromOriginal(text) {
    const newline = text.indexOf("\n")
    const kind = text.slice(0, newline), rest = text.slice(newline + 1)
    return kind === "video" ? fromVideo(JSON.parse(rest)) : fromImage(rest)
}

// A rational as EXIF writes it ("253126/37417"), or a list of them
// ("49/1,57/1,5279/100").
function number(text) {
    if (text === undefined)
        return NaN
    const [top, bottom] = String(text).split("/")
    return bottom === undefined ? Number(top) : Number(top) / Number(bottom)
}
function numbers(text) {
    return text === undefined ? [] : String(text).split(",").map(number)
}

function fromImage(text) {
    const lines = text.split("\n")
    const [format, width, height] = lines[0].split(" ")
    const exif = {}
    for (const line of lines.slice(1)) {
        const equals = line.indexOf("=")
        if (line.startsWith("exif:") && equals > 0)
            exif[line.slice(5, equals)] = line.slice(equals + 1).trim()
    }
    const facts = { format: format === "DNG" ? "DNG" : format }
    const orientation = Number(exif.Orientation)
    if (Number(width) > 0 && Number(height) > 0) {
        const turned = orientation >= 5 && orientation <= 8
        facts.width = Number(turned ? height : width)
        facts.height = Number(turned ? width : height)
    }

    const make = (exif.Make || "").trim(), model = (exif.Model || "").trim()
    facts.camera = cameraName(make, model)
    // An iPhone's lens says which iPhone first: "iPhone 16 Pro Max back
    // triple camera 6.765mm f/1.78".
    let lens = (exif.LensModel || "").trim()
    if (model !== "" && lens.startsWith(model + " "))
        lens = lens.slice(model.length + 1)
    if (lens !== "")
        facts.lens = lens[0].toUpperCase() + lens.slice(1)

    const focal = number(exif.FocalLength)
    if (focal > 0)
        facts.focal = focal
    const equivalent = number(exif.FocalLengthIn35mmFilm)
    if (equivalent > 0)
        facts.focal35 = equivalent
    const aperture = number(exif.FNumber)
    if (aperture > 0)
        facts.aperture = aperture
    const shutter = number(exif.ExposureTime)
    if (shutter > 0)
        facts.shutter = shutter
    const iso = number(exif.PhotographicSensitivity || exif.ISOSpeedRatings)
    if (iso > 0)
        facts.iso = iso

    // When, as the camera's clock said, and how far from UTC it was.
    const taken = exif.DateTimeOriginal || exif.DateTimeDigitized || exif.DateTime
    if (taken)
        facts.taken = { local: taken.replace(/^(\d+):(\d+):(\d+)/, "$1-$2-$3"), offset: exif.OffsetTimeOriginal || exif.OffsetTime || "" }

    const lat = numbers(exif.GPSLatitude), lon = numbers(exif.GPSLongitude)
    if (lat.length === 3 && lon.length === 3 && lat.concat(lon).every(isFinite)) {
        const latitude = (lat[0] + lat[1] / 60 + lat[2] / 3600) * (exif.GPSLatitudeRef === "S" ? -1 : 1)
        const longitude = (lon[0] + lon[1] / 60 + lon[2] / 3600) * (exif.GPSLongitudeRef === "W" ? -1 : 1)
        // 0, 0 is a phone that had no fix and wrote it anyway.
        if (latitude !== 0 || longitude !== 0) {
            facts.latitude = latitude
            facts.longitude = longitude
            // The reference is a byte: 0 above sea level (ImageMagick
            // shows it as "."), 1 below.
            const altitude = number(exif.GPSAltitude)
            if (isFinite(altitude))
                facts.altitude = exif.GPSAltitudeRef === "1" ? -altitude : altitude
        }
    }
    // Only a yes: the index may know a place the file does not hold.
    if (facts.latitude !== undefined)
        facts.hasLocation = true
    return facts
}

function cameraName(make, model) {
    if (model === "")
        return make
    // "Canon" and "Canon EOS R5", "NIKON CORPORATION" and "NIKON Z 6";
    // "Apple" and "iPhone 16 Pro Max", a name of its own.
    const brand = make.split(" ")[0].toLowerCase()
    return brand === "" || brand === "apple" || model.toLowerCase().startsWith(brand) ? model : make + " " + model
}

const codecs = {
    hevc: "HEVC", h264: "H.264", av1: "AV1", vp9: "VP9", vp8: "VP8", prores: "ProRes", mpeg4: "MPEG-4",
    mjpeg: "Motion JPEG", mpeg2video: "MPEG-2", aac: "AAC", alac: "ALAC", opus: "Opus", mp3: "MP3",
    pcm_s16le: "PCM", ac3: "AC-3", eac3: "E-AC-3", apple_apac: "APAC", flac: "FLAC", vorbis: "Vorbis"
}
function codecName(name) { return codecs[name] || String(name).toUpperCase() }

function fromVideo(probe) {
    const streams = probe.streams || [], format = probe.format || {}
    const tags = format.tags || {}
    const facts = {}
    const video = streams.find(stream => stream.codec_type === "video" && !(stream.disposition && stream.disposition.attached_pic))
    const audio = streams.find(stream => stream.codec_type === "audio")
    const names = (format.format_name || "").split(",")
    facts.format = names.indexOf("mov") >= 0 ? (/\.mp4$/i.test(format.filename) ? "MP4" : "MOV")
                 : names.indexOf("matroska") >= 0 ? "MKV" : String(names[0]).toUpperCase()
    if (video) {
        // Turned by its display matrix: as it is shown.
        let rotation = 0
        for (const side of video.side_data_list || [])
            if (side.rotation !== undefined)
                rotation = Number(side.rotation)
        if (video.tags && video.tags.rotate)
            rotation = Number(video.tags.rotate)
        const turned = Math.abs(rotation) % 180 === 90
        if (video.width > 0 && video.height > 0) {
            facts.width = turned ? video.height : video.width
            facts.height = turned ? video.width : video.height
        }
        facts.codec = codecName(video.codec_name)
        const sides = (video.side_data_list || []).map(side => side.side_data_type)
        if (sides.indexOf("DOVI configuration record") >= 0)
            facts.hdr = "Dolby Vision"
        else if (video.color_transfer === "arib-std-b67")
            facts.hdr = "HLG"
        else if (video.color_transfer === "smpte2084")
            facts.hdr = "HDR10"
        const [top, bottom] = String(video.avg_frame_rate || video.r_frame_rate || "0/0").split("/").map(Number)
        if (top > 0 && bottom > 0)
            facts.fps = top / bottom
    }
    if (audio)
        facts.audio = codecName(audio.codec_name)
    const duration = Number(format.duration || (video && video.duration))
    if (duration > 0)
        facts.duration = duration
    if (Number(format.size) > 0)
        facts.bytes = Number(format.size)

    // An iPhone's, else an Android phone's.
    const make = tags["com.apple.quicktime.make"] || tags["com.android.manufacturer"] || tags.make || ""
    const model = tags["com.apple.quicktime.model"] || tags["com.android.model"] || tags.model || ""
    facts.camera = cameraName(make.trim(), model.trim())

    // "2026-10-28T11:13:37+0100", as the phone's clock said; else when
    // it was written, in UTC.
    const local = tags["com.apple.quicktime.creationdate"]
    const match = local ? local.match(/^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(?:\.\d+)?([+-]\d\d):?(\d\d)$/) : null
    if (match)
        facts.taken = { local: match[1].replace("T", " "), offset: match[2] + ":" + match[3] }

    // ISO 6709: "+49.9821+018.9293+257.055/".
    const place = tags["com.apple.quicktime.location.ISO6709"] || tags.location || ""
    const at = place.match(/^([+-]\d+(?:\.\d+)?)([+-]\d+(?:\.\d+)?)([+-]\d+(?:\.\d+)?)?/)
    if (at && (Number(at[1]) !== 0 || Number(at[2]) !== 0)) {
        facts.latitude = Number(at[1])
        facts.longitude = Number(at[2])
        if (at[3] !== undefined)
            facts.altitude = Number(at[3])
    }
    // Only a yes: the index may know a place the file does not hold.
    if (facts.latitude !== undefined)
        facts.hasLocation = true
    return facts
}

// The facts known so far: what came later over what came first, but only
// where it says something.
function merged(first, later) {
    const all = Object.assign({}, first)
    for (const key in later)
        if (later[key] !== undefined && later[key] !== "")
            all[key] = later[key]
    // The same size both ways round: the index's way, turned as the photo
    // is shown. A HEIC is turned by a box of its own, which its EXIF does
    // not say, and ImageMagick gives it as it is stored.
    if (first.width === later.height && first.height === later.width) {
        all.width = first.width
        all.height = first.height
    }
    return all
}

// Numbers as the panel writes them.
function decimal(value, places) {
    const text = value.toFixed(places)
    return text.indexOf(".") >= 0 ? text.replace(/\.?0+$/, "") : text
}
function shutterText(seconds) {
    if (seconds >= 1)
        return decimal(seconds, 1) + " s"
    // 1/46, not 1/45.98.
    return "1/" + Math.round(1 / seconds) + " s"
}
function clock(seconds) {
    const whole = Math.round(seconds)
    const h = Math.floor(whole / 3600), m = Math.floor(whole / 60) % 60, s = whole % 60
    const two = n => (n < 10 ? "0" : "") + n
    return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s)
}
function fpsText(fps) {
    // 59.94 is what NTSC's 60 is, and every phone's "60 fps"; an iPhone's,
    // its frame rate varying, averages 59.89.
    return (Math.abs(fps - Math.round(fps)) < 0.25 ? Math.round(fps) : decimal(fps, 2)) + " fps"
}
function coordinates(facts) {
    const one = (value, plus, minus) => decimal(Math.abs(value), 5) + "° " + (value < 0 ? minus : plus)
    return one(facts.latitude, "N", "S") + ", " + one(facts.longitude, "E", "W")
}
// OpenStreetMap at the place, a pin on it.
function mapUrl(facts) {
    const lat = decimal(facts.latitude, 6), lon = decimal(facts.longitude, 6)
    return "https://www.openstreetmap.org/?mlat=" + lat + "&mlon=" + lon + "#map=16/" + lat + "/" + lon
}
