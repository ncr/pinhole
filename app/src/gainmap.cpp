#include "gainmap.h"

#include "colour.h"

#include <QFile>
#include <QImageReader>
#include <QRegularExpression>
#include <QTransform>

#include <cstring>

#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <ultrahdr_api.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr char kAppleAux[] = "urn:com:apple:photo:2020:aux:hdrgainmap";
constexpr char kIsoName[] = "urn:iso:std:iso:ts:21496:-1";
constexpr char kXmpName[] = "http://ns.adobe.com/xap/1.0/";

// Bytes read in order, big-endian unless said, never past the end.
struct Reader {
    const uchar *data;
    qsizetype size;
    qsizetype at = 0;
    bool little = false;
    bool failed = false;

    bool has(qsizetype n) const { return !failed && n >= 0 && at + n <= size; }
    quint32 read(int bytes) {
        if (!has(bytes)) {
            failed = true;
            return 0;
        }
        quint32 v = 0;
        for (int i = 0; i < bytes; ++i)
            v = little ? v | quint32(data[at + i]) << (8 * i) : v << 8 | data[at + i];
        at += bytes;
        return v;
    }
    quint8 u8() { return quint8(read(1)); }
    quint16 u16() { return quint16(read(2)); }
    quint32 u32() { return read(4); }
    qint32 s32() { return qint32(read(4)); }
};

// A value of a TIFF entry as a number: the first of a rational, signed or
// not, an integer or a float. `base` is where its offsets count from.
bool tiffNumber(const uchar *data, qsizetype size, bool little, qsizetype base, qsizetype entry, double *out) {
    Reader r{data, size, entry + 2, little};
    const quint16 type = r.u16();
    const quint32 count = r.u32();
    if (r.failed || count < 1)
        return false;
    const qsizetype valueAt = r.at;
    auto at = [&](int bytes) -> qsizetype {
        if (bytes <= 4)
            return valueAt;
        Reader o{data, size, valueAt, little};
        return base + qsizetype(o.u32());
    };
    switch (type) {
    case 3: {
        Reader v{data, size, at(2), little};
        *out = v.u16();
        return !v.failed;
    }
    case 4:
    case 9: {
        Reader v{data, size, at(4), little};
        *out = type == 4 ? double(v.u32()) : double(v.s32());
        return !v.failed;
    }
    case 5:
    case 10: {
        Reader v{data, size, at(8), little};
        const double n = type == 5 ? double(v.u32()) : double(v.s32());
        const double d = type == 5 ? double(v.u32()) : double(v.s32());
        if (v.failed || d == 0)
            return false;
        *out = n / d;
        return true;
    }
    case 11: {
        Reader v{data, size, at(4), little};
        const quint32 bits = v.u32();
        float f;
        std::memcpy(&f, &bits, 4);
        *out = f;
        return !v.failed;
    }
    default:
        return false;
    }
}

// The entry of `tag` in the IFD at `ifd`, or -1.
qsizetype tiffEntry(const uchar *data, qsizetype size, bool little, qsizetype ifd, quint16 tag) {
    Reader r{data, size, ifd, little};
    const quint16 count = r.u16();
    for (int i = 0; i < count && !r.failed; ++i) {
        const qsizetype entry = r.at;
        if (r.u16() == tag)
            return r.failed ? -1 : entry;
        r.at = entry + 12;
    }
    return -1;
}

// A number from the XMP: an attribute or an element of that name.
bool xmpNumber(const QByteArray &xmp, const char *name, double *out) {
    const QRegularExpression re(QStringLiteral("%1\\s*(?:=\\s*[\"']|>)\\s*([-+0-9.eE]+)").arg(QRegularExpression::escape(QLatin1String(name))));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(xmp));
    if (!m.hasMatch())
        return false;
    bool ok = false;
    *out = m.captured(1).toDouble(&ok);
    return ok && std::isfinite(*out);
}

// What an XMP packet says of a gain map: Apple's, or Adobe's (ISO's maths).
void readXmp(const QByteArray &xmp, HdrInfo &info) {
    double headroom = 0;
    if (xmp.contains("HDRGainMapVersion") || xmp.contains("HDRGainMapHeadroom")) {
        info.apple = true;
        if (xmpNumber(xmp, "HDRGainMap:HDRGainMapHeadroom", &headroom) && headroom > 0)
            info.xmpStops = std::max(0.0, std::log2(headroom));
    }
    if (xmp.contains("hdrgm:")) {
        // Its alternate's headroom, in stops already.
        if (xmpNumber(xmp, "hdrgm:HDRCapacityMax", &headroom)) {
            info.iso = true;
            info.isoStops = std::max(info.isoStops, headroom);
        }
    }
}

void finish(HdrInfo &info) {
    if (info.apple)
        info.appleStops = info.xmpStops >= 0 ? info.xmpStops : std::max(0.0, info.makerStops);
}

void readExif(const QByteArray &exif, HdrInfo &info) {
    double m33 = 0, m48 = 0;
    if (appleMakerTags(exif, &m33, &m48))
        info.makerStops = appleMakerStops(m33, m48);
}

// ---- HEIF ----

void readHeif(const QString &path, HdrInfo &info) {
    heif_context *context = heif_context_alloc();
    heif_image_handle *primary = nullptr;
    if (heif_context_read_from_file(context, QFile::encodeName(path).constData(), nullptr).code != heif_error_Ok ||
        heif_context_get_primary_image_handle(context, &primary).code != heif_error_Ok) {
        heif_context_free(context);
        return;
    }
    // The primary's own curve.
    heif_color_profile_nclx *nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(primary, &nclx).code == heif_error_Ok && nclx) {
        if (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ) {
            info.transfer = QStringLiteral("pq");
            info.transferStops = std::log2(pqPeakOf(primary) / kSdrWhiteNits);
        } else if (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG) {
            info.transfer = QStringLiteral("hlg");
            info.transferStops = std::log2(kHlgPeakNits / kSdrWhiteNits);
        }
        heif_nclx_color_profile_free(nclx);
    }
    // Apple's map: an auxiliary image of the primary.
    std::vector<heif_item_id> described = {heif_image_handle_get_item_id(primary)};
    const int auxFilter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA | LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH;
    const int auxCount = heif_image_handle_get_number_of_auxiliary_images(primary, auxFilter);
    std::vector<heif_item_id> aux(size_t(std::max(auxCount, 0)));
    heif_image_handle_get_list_of_auxiliary_image_IDs(primary, auxFilter, aux.data(), auxCount);
    for (heif_item_id id : aux) {
        heif_image_handle *handle = nullptr;
        if (heif_image_handle_get_auxiliary_image_handle(primary, id, &handle).code != heif_error_Ok)
            continue;
        const char *type = nullptr;
        if (heif_image_handle_get_auxiliary_type(handle, &type).code == heif_error_Ok && type) {
            if (qstrcmp(type, kAppleAux) == 0) {
                info.apple = true;
                described.push_back(id);
            }
            heif_image_handle_release_auxiliary_type(handle, &type);
        }
        heif_image_handle_release(handle);
    }
    // The items: the Exif and XMP of the primary or of Apple's map (found
    // by their references: libheif gives none to a handle when one
    // describes two items, as an iPhone's Exif does), and ISO's tmap.
    const int items = heif_context_get_number_of_items(context);
    std::vector<heif_item_id> ids(size_t(std::max(items, 0)));
    heif_context_get_list_of_item_IDs(context, ids.data(), items);
    auto describes = [&](heif_item_id id) {
        for (int index = 0;; ++index) {
            uint32_t type = 0;
            heif_item_id *to = nullptr;
            const size_t count = heif_context_get_item_references(context, id, index, &type, &to);
            if (count == 0)
                return false;
            bool found = false;
            for (size_t i = 0; i < count && type == heif_fourcc('c', 'd', 's', 'c'); ++i)
                found = found || std::find(described.begin(), described.end(), to[i]) != described.end();
            heif_release_item_references(context, &to);
            if (found)
                return true;
        }
    };
    for (heif_item_id id : ids) {
        const uint32_t type = heif_item_get_item_type(context, id);
        const bool tmap = type == heif_fourcc('t', 'm', 'a', 'p');
        const bool exif = type == heif_item_type_exif;
        const bool xmp = type == heif_item_type_mime
                      && qstrcmp(heif_item_get_mime_item_content_type(context, id), "application/rdf+xml") == 0;
        if (!tmap && !((exif || xmp) && describes(id)))
            continue;
        uint8_t *bytes = nullptr;
        size_t size = 0;
        if (heif_item_get_item_data(context, id, nullptr, &bytes, &size).code == heif_error_Ok && bytes && size > 1
            && size < (4u << 20)) {
            const QByteArray data(reinterpret_cast<const char *>(bytes), qsizetype(size));
            if (tmap) {
                // Its metadata after a version byte.
                double stops = 0;
                if (data[0] == 0 && isoGainMapStops(data.mid(1), &stops)) {
                    info.iso = true;
                    info.isoStops = stops;
                }
            } else if (exif) {
                // Four bytes first: where in it the TIFF header is.
                Reader r{reinterpret_cast<const uchar *>(data.constData()), data.size()};
                const quint32 skip = r.u32();
                if (!r.failed && skip < quint32(data.size()) - 4)
                    readExif(data.mid(4 + qsizetype(skip)), info);
            } else {
                readXmp(data, info);
            }
        }
        if (bytes)
            heif_release_item_data(context, &bytes);
    }
    heif_image_handle_release(primary);
    heif_context_free(context);
}

// ---- JPEG ----

// The APP segments of the JPEG starting at `start`, up to its first scan:
// each marker and payload handed to `segment`. Only APP1 and APP2 are read;
// the rest are passed over.
template <typename F>
bool jpegSegments(QFile &file, qint64 start, F segment) {
    if (!file.seek(start))
        return false;
    uchar soi[2];
    if (file.read(reinterpret_cast<char *>(soi), 2) != 2 || soi[0] != 0xff || soi[1] != 0xd8)
        return false;
    for (int n = 0; n < 64; ++n) {
        uchar head[4];
        if (file.read(reinterpret_cast<char *>(head), 2) != 2 || head[0] != 0xff)
            return true;
        const uchar marker = head[1];
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01)
            continue;
        if (marker == 0xda || marker == 0xd9)
            return true;
        if (file.read(reinterpret_cast<char *>(head + 2), 2) != 2)
            return true;
        const int length = head[2] << 8 | head[3];
        if (length < 2)
            return true;
        const qint64 payloadAt = file.pos();
        if (marker == 0xe1 || marker == 0xe2) {
            const QByteArray payload = file.read(length - 2);
            if (payload.size() != length - 2)
                return true;
            segment(marker, payload, payloadAt);
        }
        if (!file.seek(payloadAt + length - 2))
            return true;
    }
    return true;
}

void readJpegSegment(uchar marker, const QByteArray &payload, HdrInfo &info, bool gainMapImage) {
    if (marker == 0xe1 && payload.startsWith("Exif\0\0")) {
        readExif(payload.mid(6), info);
    } else if (marker == 0xe1 && payload.startsWith(QByteArray(kXmpName) + '\0')) {
        readXmp(payload.mid(qsizetype(sizeof kXmpName)), info);
    } else if (marker == 0xe2 && gainMapImage && payload.startsWith(QByteArray(kIsoName) + '\0')) {
        double stops = 0;
        if (isoGainMapStops(payload.mid(qsizetype(sizeof kIsoName)), &stops)) {
            info.iso = true;
            info.isoStops = stops;
        }
    }
}

// The second image of an MPF index (an APP2 "MPF\0" payload, read at
// `at` in the file): where it starts in the file and its size. Its
// offsets count from the MPF TIFF header.
void mpfSecond(uchar marker, const QByteArray &payload, qint64 at, qint64 *second, qint64 *length) {
    if (marker != 0xe2 || !payload.startsWith(QByteArray("MPF\0", 4)) || payload.size() < 12)
        return;
    const auto *tiff = reinterpret_cast<const uchar *>(payload.constData()) + 4;
    const qsizetype size = payload.size() - 4;
    const bool little = tiff[0] == 'I';
    Reader r{tiff, size, 4, little};
    const qsizetype ifd = qsizetype(r.u32());
    const qsizetype entry = r.failed ? -1 : tiffEntry(tiff, size, little, ifd, 0xb002);
    if (entry < 0)
        return;
    Reader e{tiff, size, entry + 4, little};
    const quint32 count = e.u32();
    e.at = qsizetype(e.u32());
    if (e.failed || count < 32)
        return;
    // The second of 16-byte entries: attribute, size, offset.
    e.at += 16 + 4;
    const quint32 bytes = e.u32();
    const quint32 offset = e.u32();
    if (!e.failed && offset > 0) {
        *second = at + 4 + qint64(offset);
        *length = qint64(bytes);
    }
}

void readJpeg(const QString &path, HdrInfo &info) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    // The second image's offset, from the MPF index: counted from the MPF
    // TIFF header.
    qint64 second = -1;
    const bool jpeg = jpegSegments(file, 0, [&](uchar marker, const QByteArray &payload, qint64 at) {
        readJpegSegment(marker, payload, info, false);
        qint64 size = 0;
        mpfSecond(marker, payload, at, &second, &size);
    });
    if (jpeg && second > 0)
        jpegSegments(file, second, [&](uchar marker, const QByteArray &payload, qint64) { readJpegSegment(marker, payload, info, true); });
}

} // namespace

QString HdrInfo::kind() const {
    if (iso)
        return QStringLiteral("iso");
    if (apple)
        return QStringLiteral("apple");
    return transfer;
}

double HdrInfo::headroom() const {
    if (iso)
        return isoStops;
    if (apple)
        return appleStops;
    return transferStops;
}

QVariantMap HdrInfo::toMap() const {
    const QString k = kind();
    if (k.isEmpty())
        return {};
    return {{QStringLiteral("kind"), k}, {QStringLiteral("headroom"), std::round(headroom() * 100) / 100}};
}

double appleMakerStops(double m33, double m48) {
    double stops;
    if (m33 < 1)
        stops = m48 <= 0.01 ? -20 * m48 + 1.8 : -0.101 * m48 + 1.601;
    else
        stops = m48 <= 0.01 ? -70 * m48 + 3.0 : -0.303 * m48 + 2.303;
    return std::max(stops, 0.0);
}

bool appleMakerTags(const QByteArray &exifBytes, double *m33, double *m48) {
    const QByteArray exif = exifBytes.startsWith(QByteArray("Exif\0\0", 6)) ? exifBytes.mid(6) : exifBytes;
    const auto *tiff = reinterpret_cast<const uchar *>(exif.constData());
    const qsizetype size = exif.size();
    if (size < 8 || !(exif.startsWith("II") || exif.startsWith("MM")))
        return false;
    const bool little = tiff[0] == 'I';
    Reader r{tiff, size, 4, little};
    const qsizetype ifd0 = qsizetype(r.u32());
    const qsizetype exifPointer = r.failed ? -1 : tiffEntry(tiff, size, little, ifd0, 0x8769);
    if (exifPointer < 0)
        return false;
    Reader p{tiff, size, exifPointer + 8, little};
    const qsizetype exifIfd = qsizetype(p.u32());
    const qsizetype maker = p.failed ? -1 : tiffEntry(tiff, size, little, exifIfd, 0x927c);
    if (maker < 0)
        return false;
    Reader m{tiff, size, maker + 4, little};
    const qsizetype makerSize = qsizetype(m.u32());
    const qsizetype makerAt = qsizetype(m.u32());
    if (m.failed || makerAt < 0 || makerSize < 16 || makerAt + makerSize > size)
        return false;
    // "Apple iOS\0", a version, "MM", then the IFD; offsets count from
    // the note's start.
    const uchar *note = tiff + makerAt;
    if (std::memcmp(note, "Apple iOS", 9) != 0)
        return false;
    const bool noteLittle = note[12] == 'I';
    const qsizetype e33 = tiffEntry(note, makerSize, noteLittle, 14, 33);
    const qsizetype e48 = tiffEntry(note, makerSize, noteLittle, 14, 48);
    if (e33 < 0 || e48 < 0)
        return false;
    return tiffNumber(note, makerSize, noteLittle, 0, e33, m33) && tiffNumber(note, makerSize, noteLittle, 0, e48, m48);
}

bool isoGainMapMetadata(const QByteArray &metadata, GainMap *map) {
    Reader r{reinterpret_cast<const uchar *>(metadata.constData()), metadata.size()};
    const quint16 minimumVersion = r.u16();
    r.u16(); // the writer's version
    const quint8 flags = r.u8();
    if (r.failed || minimumVersion != 0)
        return false;
    // Multi-channel, the base's colour space, the base the HDR one, one
    // denominator for all.
    const int channels = (flags & 0x80) ? 3 : 1;
    const bool common = flags & 0x08;
    GainMap m;
    m.useBaseColourSpace = flags & 0x40;
    m.backward = flags & 0x04;
    // A fraction: its numerator signed or not, over the common denominator
    // or its own.
    quint32 denominator = 0;
    auto fraction = [&](bool isSigned, double *out) {
        const quint32 n = r.u32();
        const quint32 d = common ? denominator : r.u32();
        if (r.failed || d == 0) {
            r.failed = true;
            return;
        }
        *out = (isSigned ? double(qint32(n)) : double(n)) / d;
    };
    if (common)
        denominator = r.u32();
    fraction(false, &m.baseHeadroom);
    fraction(false, &m.alternateHeadroom);
    for (int c = 0; c < channels; ++c) {
        fraction(true, &m.gainMin[c]);
        fraction(true, &m.gainMax[c]);
        fraction(false, &m.gamma[c]);
        fraction(true, &m.baseOffset[c]);
        fraction(true, &m.alternateOffset[c]);
        if (!r.failed && m.gamma[c] <= 0)
            r.failed = true;
    }
    if (r.failed || !std::isfinite(m.baseHeadroom) || !std::isfinite(m.alternateHeadroom))
        return false;
    for (int c = channels; c < 3; ++c) {
        m.gainMin[c] = m.gainMin[0], m.gainMax[c] = m.gainMax[0], m.gamma[c] = m.gamma[0];
        m.baseOffset[c] = m.baseOffset[0], m.alternateOffset[c] = m.alternateOffset[0];
    }
    m.kind = QStringLiteral("iso");
    m.image = map->image;
    *map = m;
    return true;
}

bool isoGainMapStops(const QByteArray &metadata, double *stops) {
    GainMap map;
    if (!isoGainMapMetadata(metadata, &map))
        return false;
    *stops = std::max(map.baseHeadroom, map.alternateHeadroom);
    return std::isfinite(*stops);
}

HdrInfo readHdr(const QString &path) {
    HdrInfo info;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return info;
    const QByteArray head = file.read(12);
    file.close();
    if (head.startsWith("\xff\xd8"))
        readJpeg(path, info);
    else if (head.size() == 12 && head.mid(4, 4) == "ftyp")
        readHeif(path, info);
    finish(info);
    return info;
}

// ---- The map's pixels (H4) ----

double GainMap::weight(double stops) const {
    if (kind == QLatin1String("apple")) {
        const double top = std::log2(appleHeadroom);
        return top > 0 ? std::clamp(stops / top, 0.0, 1.0) : 0;
    }
    if (kind != QLatin1String("iso") || alternateHeadroom == baseHeadroom)
        return 0;
    const double f = std::clamp((stops - baseHeadroom) / (alternateHeadroom - baseHeadroom), 0.0, 1.0);
    return alternateHeadroom < baseHeadroom ? -f : f;
}

double GainMap::gainOf(double code, int c) const {
    code = std::clamp(code, 0.0, 1.0);
    if (kind == QLatin1String("apple")) {
        // Rec. 709's curve undone, then Apple's 1 + (Hi - 1) g.
        const double g = code <= 0.081 ? code / 4.5 : std::pow((code + 0.099) / 1.099, 1 / 0.45);
        return std::log2(1 + (appleHeadroom - 1) * g);
    }
    return gainMin[c] + (gainMax[c] - gainMin[c]) * std::pow(code, 1 / gamma[c]);
}

void GainMap::apply(double rgb[3], const double codes[3], double w) const {
    if (w == 0 || kind.isEmpty())
        return;
    const bool apple = kind == QLatin1String("apple");
    for (int c = 0; c < 3; ++c) {
        const double gain = std::exp2(gainOf(codes[apple ? 0 : c], c) * w);
        const double out = apple ? rgb[c] * gain : (rgb[c] + baseOffset[c]) * gain - alternateOffset[c];
        // Not taken below black by the offsets (as libultrahdr has it); a
        // colour beyond sRGB's, below 0 already, as it comes.
        rgb[c] = rgb[c] >= 0 ? std::max(out, 0.0) : out;
    }
}

namespace {

// Turned upright as Qt's readers turn a picture (qt_imageTransform).
QImage upright(QImage image, QImageIOHandler::Transformations t) {
    if (image.isNull() || t == QImageIOHandler::TransformationNone)
        return image;
    if (t == QImageIOHandler::TransformationRotate270)
        return image.transformed(QTransform().rotate(270));
    image = image.flipped((t & QImageIOHandler::TransformationMirror ? Qt::Horizontal : Qt::Orientations())
                          | (t & QImageIOHandler::TransformationFlip ? Qt::Vertical : Qt::Orientations()));
    if (t & QImageIOHandler::TransformationRotate90)
        image = image.transformed(QTransform().rotate(90));
    return image;
}

// A map's codes as kept: one channel, or three where they differ.
QImage mapCodes(const QImage &image) {
    if (image.isNull())
        return {};
    if (!image.allGray())
        return image.convertToFormat(QImage::Format_RGB888);
    return image.convertToFormat(QImage::Format_Grayscale8);
}

// A HEIF image item's pixels at 8 bits, one channel or three.
QImage decodeHeifMap(heif_image_handle *handle, bool colour, const std::atomic<bool> *cancelled) {
    heif_decoding_options *options = heif_decoding_options_alloc();
    options->convert_hdr_to_8bit = 1;
    if (cancelled) {
        options->progress_user_data = const_cast<std::atomic<bool> *>(cancelled);
        options->cancel_decoding = [](void *data) { return int(static_cast<std::atomic<bool> *>(data)->load()); };
    }
    heif_image *image = nullptr;
    QImage out;
    // libheif applies the item's own rotation and mirroring, which an
    // iPhone gives its map as it gives the photo (the same irot).
    if (heif_decode_image(handle, &image, colour ? heif_colorspace_RGB : heif_colorspace_monochrome,
                          colour ? heif_chroma_interleaved_RGB : heif_chroma_monochrome, options)
            .code == heif_error_Ok) {
        const heif_channel channel = colour ? heif_channel_interleaved : heif_channel_Y;
        size_t stride = 0;
        const uint8_t *data = heif_image_get_plane_readonly2(image, channel, &stride);
        if (data && heif_image_get_bits_per_pixel_range(image, channel) == 8)
            out = QImage(data, heif_image_get_width(image, channel), heif_image_get_height(image, channel), qsizetype(stride),
                         colour ? QImage::Format_RGB888 : QImage::Format_Grayscale8)
                      .copy();
        heif_image_release(image);
    }
    heif_decoding_options_free(options);
    return out;
}

bool sameChannels(const GainMap &m) {
    for (int c = 1; c < 3; ++c)
        if (m.gainMin[c] != m.gainMin[0] || m.gainMax[c] != m.gainMax[0] || m.gamma[c] != m.gamma[0])
            return false;
    return true;
}

void readHeifMap(const QString &path, const HdrInfo &info, bool appleFirst, GainMap &map, const std::atomic<bool> *cancelled) {
    heif_context *context = heif_context_alloc();
    heif_image_handle *primary = nullptr;
    if (heif_context_read_from_file(context, QFile::encodeName(path).constData(), nullptr).code != heif_error_Ok ||
        heif_context_get_primary_image_handle(context, &primary).code != heif_error_Ok) {
        heif_context_free(context);
        return;
    }
    const heif_item_id primaryId = heif_image_handle_get_item_id(primary);
    // ISO's: a tmap item whose two inputs (dimg) are the primary and the
    // map. libheif 1.23 has no API for it: its metadata read here.
    auto readIso = [&] {
        if (!info.iso)
            return false;
        const int items = heif_context_get_number_of_items(context);
        std::vector<heif_item_id> ids(size_t(std::max(items, 0)));
        heif_context_get_list_of_item_IDs(context, ids.data(), items);
        for (heif_item_id id : ids) {
            if (heif_item_get_item_type(context, id) != heif_fourcc('t', 'm', 'a', 'p'))
                continue;
            heif_item_id inputs[2] = {0, 0};
            for (int index = 0;; ++index) {
                uint32_t type = 0;
                heif_item_id *to = nullptr;
                const size_t count = heif_context_get_item_references(context, id, index, &type, &to);
                if (count == 0)
                    break;
                if (type == heif_fourcc('d', 'i', 'm', 'g') && count == 2)
                    inputs[0] = to[0], inputs[1] = to[1];
                heif_release_item_references(context, &to);
            }
            if (inputs[0] != primaryId || inputs[1] == 0)
                continue;
            uint8_t *bytes = nullptr;
            size_t size = 0;
            GainMap iso;
            const bool parsed = heif_item_get_item_data(context, id, nullptr, &bytes, &size).code == heif_error_Ok && bytes
                             && size > 1 && bytes[0] == 0
                             && isoGainMapMetadata(QByteArray(reinterpret_cast<const char *>(bytes) + 1, qsizetype(size) - 1), &iso);
            if (bytes)
                heif_release_item_data(context, &bytes);
            if (!parsed || iso.backward)
                continue;
            // The map: an image item, here (an iPhone's) Apple's own
            // auxiliary image of the primary too.
            heif_image_handle *handle = nullptr;
            if (heif_context_get_image_handle(context, inputs[1], &handle).code != heif_error_Ok)
                heif_image_handle_get_auxiliary_image_handle(primary, inputs[1], &handle);
            if (!handle)
                continue;
            iso.image = mapCodes(decodeHeifMap(handle, !sameChannels(iso), cancelled));
            heif_image_handle_release(handle);
            if (!iso.image.isNull()) {
                map = iso;
                return true;
            }
        }
        return false;
    };
    // Apple's: the primary's auxiliary image of its kind.
    auto readApple = [&] {
        if (!info.apple || info.appleStops <= 0)
            return false;
        const int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA | LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH;
        const int count = heif_image_handle_get_number_of_auxiliary_images(primary, filter);
        std::vector<heif_item_id> aux(size_t(std::max(count, 0)));
        heif_image_handle_get_list_of_auxiliary_image_IDs(primary, filter, aux.data(), count);
        for (heif_item_id id : aux) {
            heif_image_handle *handle = nullptr;
            if (heif_image_handle_get_auxiliary_image_handle(primary, id, &handle).code != heif_error_Ok)
                continue;
            const char *type = nullptr;
            bool apple = false;
            if (heif_image_handle_get_auxiliary_type(handle, &type).code == heif_error_Ok && type) {
                apple = qstrcmp(type, kAppleAux) == 0;
                heif_image_handle_release_auxiliary_type(handle, &type);
            }
            GainMap found;
            if (apple) {
                found.kind = QStringLiteral("apple");
                found.appleHeadroom = std::exp2(info.appleStops);
                found.image = decodeHeifMap(handle, false, cancelled);
            }
            heif_image_handle_release(handle);
            if (!found.image.isNull()) {
                map = found;
                return true;
            }
        }
        return false;
    };
    if (appleFirst ? !readApple() && !readIso() : !readIso() && !readApple())
        map = GainMap();
    heif_image_handle_release(primary);
    heif_context_free(context);
}

// A JPEG's: ISO's (or Adobe's XMP) by libultrahdr, whose map is decoded
// here; else Apple's, the MPF second image.
void readJpegMap(const QString &path, const HdrInfo &info, bool appleFirst, GainMap &map) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > (256 << 20))
        return;
    const QByteArray bytes = file.readAll();
    // Upright as the photo is (QImageReader, autoTransform).
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const QImageIOHandler::Transformations turn = reader.transformation();
    auto readIso = [&] {
        if (!info.iso)
            return false;
        uhdr_codec_private_t *decoder = uhdr_create_decoder();
        if (!decoder)
            return false;
        uhdr_compressed_image_t image{const_cast<char *>(bytes.constData()), size_t(bytes.size()), size_t(bytes.size()),
                                      UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
        GainMap iso;
        if (uhdr_dec_set_image(decoder, &image).error_code == UHDR_CODEC_OK
            && uhdr_dec_probe(decoder).error_code == UHDR_CODEC_OK) {
            const uhdr_gainmap_metadata_t *m = uhdr_dec_get_gainmap_metadata(decoder);
            const uhdr_mem_block_t *coded = uhdr_dec_get_gainmap_image(decoder);
            bool sane = m && coded && coded->data && m->hdr_capacity_min > 0 && m->hdr_capacity_max > 0;
            for (int c = 0; sane && c < 3; ++c) {
                sane = m->min_content_boost[c] > 0 && m->max_content_boost[c] > 0 && m->gamma[c] > 0;
                iso.gainMin[c] = std::log2(m->min_content_boost[c]);
                iso.gainMax[c] = std::log2(m->max_content_boost[c]);
                iso.gamma[c] = m->gamma[c];
                iso.baseOffset[c] = m->offset_sdr[c];
                iso.alternateOffset[c] = m->offset_hdr[c];
            }
            if (sane) {
                iso.kind = QStringLiteral("iso");
                iso.baseHeadroom = std::log2(m->hdr_capacity_min);
                iso.alternateHeadroom = std::log2(m->hdr_capacity_max);
                iso.useBaseColourSpace = m->use_base_cg;
                const QByteArray coded8(static_cast<const char *>(coded->data), qsizetype(coded->data_sz));
                iso.image = mapCodes(upright(QImage::fromData(coded8, "JPEG"), turn));
            }
        }
        uhdr_release_decoder(decoder);
        if (iso.image.isNull())
            return false;
        map = iso;
        return true;
    };
    auto readApple = [&] {
        if (!info.apple || info.appleStops <= 0)
            return false;
        qint64 second = -1, size = 0;
        jpegSegments(file, 0, [&](uchar marker, const QByteArray &payload, qint64 at) {
            mpfSecond(marker, payload, at, &second, &size);
        });
        if (second <= 0 || size <= 0 || second + size > bytes.size())
            return false;
        GainMap apple;
        apple.kind = QStringLiteral("apple");
        apple.appleHeadroom = std::exp2(info.appleStops);
        apple.image = upright(QImage::fromData(bytes.mid(second, size), "JPEG"), turn).convertToFormat(QImage::Format_Grayscale8);
        if (apple.image.isNull())
            return false;
        map = apple;
        return true;
    };
    if (appleFirst ? !readApple() && !readIso() : !readIso() && !readApple())
        map = GainMap();
}

} // namespace

GainMap readGainMap(const QString &path, const std::atomic<bool> *cancelled) {
    GainMap map;
    const HdrInfo info = readHdr(path);
    if (!info.apple && !info.iso)
        return map;
    const bool appleFirst = qEnvironmentVariable("PINHOLE_GAINMAP") == QLatin1String("apple");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return map;
    const QByteArray head = file.read(12);
    file.close();
    if (head.startsWith("\xff\xd8"))
        readJpegMap(path, info, appleFirst, map);
    else if (head.size() == 12 && head.mid(4, 4) == "ftyp")
        readHeifMap(path, info, appleFirst, map, cancelled);
    if (cancelled && *cancelled)
        return {};
    return map;
}
