// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart reader for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#pragma once

#include <QImage>
#include <QPointF>
#include <QString>
#include <cstdint>
#include <vector>

// Reader for one BSB/KAP raster chart file.
//
// A KAP is a text header followed by a run-length-encoded, palette-indexed
// bitmap, and a row-offset index table at the very end of the file:
//
//   "BSB/NA=...,RA=2048,2048"     text header, CRLF lines, `TOK/k=v,k=v`
//   ...                           continuation lines start with whitespace
//   0x1A [0x00]                   header/raster separator
//   <depth byte>                  bits per palette index (matches IFM)
//   <row 0> <row 1> ... <row h-1> RLE rows, each = varint row number + runs
//   <h x uint32be row offsets>    the index table
//   <uint32be>                    last 4 bytes: offset of the index table
//
// The index table is what makes this format cheap to serve as tiles: a row is
// individually addressable, so rendering a tile decodes only the rows it covers
// instead of the whole (often 2048 x 6144) image. Nothing here ever holds a full
// decoded chart in memory.
//
// Georeferencing comes from the REF/ pixel<->lat/lon control points. For a
// north-up Mercator chart — which is what KAP is overwhelmingly used for, and
// the only kind this reader accepts — longitude is linear in x and *projected*
// Mercator y is linear in y, so two independent linear fits place the chart
// exactly. See parse() for the rejection rules.

// Maps chart pixels <-> geography for a north-up Mercator chart.
//   lon   = lonA  + lonB  * px
//   mercY = mercC + mercD * py     (mercY = proj::latToY(lat), metres)
struct KapGeoref {
    double lonA = 0.0, lonB = 0.0;
    double mercC = 0.0, mercD = 0.0;

    double pxToLon(double px)    const { return lonA + lonB * px; }
    double pyToMercY(double py)  const { return mercC + mercD * py; }
    double lonToPx(double lon)   const { return (lon - lonA) / lonB; }
    double mercYToPy(double m)   const { return (m - mercC) / mercD; }
    bool   valid() const { return lonB != 0.0 && mercD != 0.0; }
};

// One parsed chart: immutable after parse(), so renderTile() is const and safe
// to call concurrently from several threads (it opens its own file handle).
class KapChart {
public:
    // Parse the header and index table of `path`. Does not decode any imagery.
    // Returns false + err for unreadable files, and for charts this reader
    // deliberately declines (see the projection/skew rules in the .cpp) — the
    // caller should skip those rather than draw them in the wrong place.
    static bool parse(const QString& path, KapChart& out, QString& err);

    // Render the XYZ tile (z, x, y) — slippy-map convention, y = 0 north — into
    // a `tilePx` square ARGB image. Pixels outside the chart's coverage are left
    // transparent. Returns false + err on a read error; returns true with a null
    // `img` when the tile doesn't touch this chart at all.
    //
    // Const and thread-safe: opens its own QFile and mutates no member state.
    bool renderTile(int z, int x, int y, int tilePx, QImage& img, QString& err) const;

    const QString& path()  const { return path_; }
    const QString& name()  const { return name_; }
    int   width()          const { return width_; }
    int   height()         const { return height_; }
    int   depth()          const { return depth_; }
    double scale()         const { return scale_; }
    const KapGeoref& georef() const { return georef_; }

    // The zoom whose tile pixels match this chart's native pixels (see the
    // derivation in the .cpp). Serving above it would only upscale.
    int nativeZoom() const;

    // Chart coverage in geographic degrees, from the REF-derived corners.
    void lonLatBounds(double& minLon, double& minLat,
                      double& maxLon, double& maxLat) const;

private:
    // Decode source row `row` into `out` (palette indices, width_ entries).
    // `f` is a file handle owned by the caller (one per renderTile call).
    bool readRow(class QFile& f, int row, std::vector<uint8_t>& out) const;

    QString path_;
    QString name_;
    int     width_  = 0;
    int     height_ = 0;
    int     depth_  = 0;
    double  scale_  = 0.0;
    KapGeoref georef_;
    // index -> colour. 256 entries so a decoded byte can never index out of
    // range regardless of the file's declared depth.
    std::vector<QRgb> palette_;
    // height_ + 1 byte offsets: entry i starts row i, and entry i+1 bounds it
    // (the last entry is the index table's own offset). The sentinel is what
    // lets readRow() read an exact byte span instead of scanning.
    std::vector<quint32> rowOffsets_;
    // Coverage polygon (PLY/) in *pixel* space, or empty when the polygon covers
    // the whole image (the common case for tiled mosaics) — testing it per pixel
    // is then pure waste, so parse() drops it.
    std::vector<QPointF> plyPixels_;
};
