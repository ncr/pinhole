#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

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
