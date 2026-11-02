#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVariantMap>

#include <atomic>

// What makes a photo HDR, read from its metadata alone, no pixels
// (docs/hdr.md, H2): a gain map beside its SDR picture, Apple's or ISO
// 21496-1's (Adobe's and Android's Ultra HDR are ISO's maths), or a
// primary picture whose curve is PQ or HLG. A few kB of Exif, XMP and
// boxes are read: the HEIC's items, a JPEG's markers up to its first scan
// and those of the image its MPF index names second.
struct HdrInfo {
    // Apple's legacy map (urn:com:apple:photo:2020:aux:hdrgainmap in a
    // HEIC, the MPF second image of a JPEG): its headroom in stops, from
    // its XMP's HDRGainMapHeadroom (linear) when it has one, else from the
    // photo's MakerNote tags 33 and 48.
    bool apple = false;
    double appleStops = 0;
    // The two of Apple's, apart, for the tests; -1 for none.
    double xmpStops = -1;
    double makerStops = -1;
    // ISO 21496-1 (a HEIC's tmap item, a JPEG's APP2 urn:iso:std:iso:ts:21496:-1, or
    // Adobe's hdrgm XMP): the alternate picture's headroom, in stops.
    bool iso = false;
    double isoStops = 0;
    // "pq" or "hlg": the primary picture's own curve; its headroom is its
    // peak over SDR white (203 nits).
    QString transfer;
    double transferStops = 0;

    // "iso", "apple", "pq", "hlg" or "": ISO's when it parses, else
    // Apple's, else the curve's.
    QString kind() const;
    // In stops, of the kind.
    double headroom() const;
    // {kind, headroom} for QML: {} for SDR.
    QVariantMap toMap() const;
};

// The photo at `path`, a HEIC (or HEIF, AVIF) or a JPEG by its content.
HdrInfo readHdr(const QString &path);

// The parts, for the tests.
// The stops Apple's MakerNote tags 33 and 48 make (docs/hdr.md), at
// least 0.
double appleMakerStops(double maker33, double maker48);
// Tags 33 and 48 of Apple's MakerNote in an Exif block (a TIFF header and
// on, or "Exif\0\0" before it); false if it has none.
bool appleMakerTags(const QByteArray &exif, double *maker33, double *maker48);
// ISO 21496-1's metadata (as in a JPEG's APP2 after its name, or a tmap
// item after its version byte): the alternate headroom in stops; false if
// it does not parse.
bool isoGainMapStops(const QByteArray &metadata, double *stops);

// A gain map's pixels and how they apply (docs/hdr.md, H4), for the stage,
// which shows the photo above SDR white by it. ISO 21496-1's when it
// parses and its image decodes (a HEIC's tmap, a JPEG's by libultrahdr:
// Apple's mediums, Android's and Adobe's Ultra HDR), else Apple's legacy
// map (a HEIC's auxiliary image, a JPEG's MPF second image).
struct GainMap {
    // "iso", "apple", or "" for none.
    QString kind;
    // Its codes, upright as the photo is decoded: Grayscale8 for one
    // channel, RGB888 for three (ISO's multi-channel maps).
    QImage image;
    // ISO's, per channel: the gain's range in log2, the code's gamma, the
    // offsets; the two headrooms in log2; where the gain applies.
    double gainMin[3] = {0, 0, 0}, gainMax[3] = {0, 0, 0}, gamma[3] = {1, 1, 1};
    double baseOffset[3] = {0, 0, 0}, alternateOffset[3] = {0, 0, 0};
    double baseHeadroom = 0, alternateHeadroom = 0;
    bool useBaseColourSpace = true;
    // ISO's map whose base is the HDR one (backward direction): not shown.
    bool backward = false;
    // Apple's: its headroom, linear (Hi).
    double appleHeadroom = 1;

    bool isNull() const { return kind.isEmpty() || image.isNull(); }
    bool multichannel() const { return image.format() == QImage::Format_RGB888; }
    // Its weight at a display headroom of `stops` (log2): ISO's
    // clamp((H - base) / (alternate - base)), negative where the alternate
    // is below the base; Apple's clamp(H / log2 Hi) (docs/hdr.md, the maths).
    double weight(double stops) const;
    // The log2 gain of a code (0 to 1) of channel `c`, at full weight.
    double gainOf(double code, int c) const;
    // A pixel in linear light (1 SDR white) at weight `w`, by the codes of
    // the map there: (rgb + base offset) * 2^(gain w) - alternate offset,
    // not below 0 where it was not. Unchanged at weight 0.
    void apply(double rgb[3], const double codes[3], double w) const;
};

// The gain map of the photo at `path` (HEIC or JPEG), with its pixels;
// kind "" if it has none or they do not decode. PINHOLE_GAINMAP=apple
// takes Apple's where both are (for tests).
GainMap readGainMap(const QString &path, const std::atomic<bool> *cancelled = nullptr);
// ISO 21496-1's metadata (as isoGainMapStops takes it) into `map`'s ISO
// fields; false if it does not parse.
bool isoGainMapMetadata(const QByteArray &metadata, GainMap *map);
