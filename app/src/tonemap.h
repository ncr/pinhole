#pragma once

#include <QImage>
#include <QList>
#include <QVideoFrame>

// HDR video in SDR on the CPU (docs/hdr.md, H2), for a frame the GPU
// cannot sample (VideoView's fallback): an HLG or PQ frame tone mapped as
// a still is (colour.h): its light (HLG's OOTF at 1000 nits), BT.2390's
// EETF to SDR white at 203 nits on the largest channel, BT.2020 to sRGB,
// clipped; in bands on Qt's thread pool, as an RGB frame. Slow: a 4K60
// HLG clip takes seven cores; VideoView tone maps on the GPU otherwise.

// An HLG or PQ frame in SDR, as an RGB frame of the same size, turn and
// times; null for any other (SDR, or a format not read here). Drawn into
// one of `buffers` that no frame holds any more, if any is (a few kept).
QVideoFrame toneMapFrame(const QVideoFrame &frame, QList<QImage> *buffers = nullptr);
