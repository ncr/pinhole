#include "gainmap.h"

#include "colour.h"

#include <QFile>
#include <QRegularExpression>

#include <cstring>

#include <libheif/heif.h>
#include <libheif/heif_items.h>

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

void readJpeg(const QString &path, HdrInfo &info) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    // The second image's offset, from the MPF index: counted from the MPF
    // TIFF header.
    qint64 second = -1;
    const bool jpeg = jpegSegments(file, 0, [&](uchar marker, const QByteArray &payload, qint64 at) {
        readJpegSegment(marker, payload, info, false);
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
        e.at += 16 + 8;
        const quint32 offset = e.u32();
        if (!e.failed && offset > 0)
            second = at + 4 + qint64(offset);
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

bool isoGainMapStops(const QByteArray &metadata, double *stops) {
    Reader r{reinterpret_cast<const uchar *>(metadata.constData()), metadata.size()};
    const quint16 minimumVersion = r.u16();
    r.u16(); // the writer's version
    const quint8 flags = r.u8();
    if (r.failed || minimumVersion != 0)
        return false;
    const bool common = flags & 0x08;
    double base, alternate;
    if (common) {
        const quint32 d = r.u32();
        const quint32 b = r.u32(), a = r.u32();
        if (r.failed || d == 0)
            return false;
        base = double(b) / d;
        alternate = double(a) / d;
    } else {
        const quint32 bn = r.u32(), bd = r.u32(), an = r.u32(), ad = r.u32();
        if (r.failed || bd == 0 || ad == 0)
            return false;
        base = double(bn) / bd;
        alternate = double(an) / ad;
    }
    // The channels must be there too, for it to be whole.
    const int channels = (flags & 0x80) ? 3 : 1;
    if (!r.has(qsizetype(channels) * (common ? 20 : 40)))
        return false;
    *stops = std::max(base, alternate);
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
