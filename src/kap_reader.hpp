// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart reader for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#pragma once

#include <QImage>
#include <QPointF>
#include <QString>
#include <cmath>
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
// Georeferencing comes from the REF/ pixel<->lat/lon control points. A KAP is a
// Mercator chart, but its raster is not necessarily north-up: BSB charts carry a
// skew angle (KNP/SK) and NOAA in particular rotates approach and harbour charts
// to run a channel or coastline up the page. In *projected* Mercator space that
// rotation is an affine map, so both world axes depend on both pixel axes:
//
//   X = xA + xB*px + xC*py          X = proj::lonToX(lon)   (metres)
//   Y = yA + yB*px + yC*py          Y = proj::latToY(lat)   (metres)
//
// A least-squares affine fit over the REF points places any skewed Mercator chart
// exactly, and collapses to the separable north-up case (xC = yB = 0) on its own
// when the chart isn't rotated. Only genuinely different projections (polyconic)
// are declined — see parse().
struct KapGeoref {
    // Forward map, chart pixel -> Mercator metres.
    double xA = 0.0, xB = 0.0, xC = 0.0;
    double yA = 0.0, yB = 0.0, yC = 0.0;
    // Determinant of the 2x2 linear part, cached by finalize() for the inverse
    // and for the pixel scale. Zero means the fit was degenerate.
    double det = 0.0;

    void finalize() { det = xB * yC - xC * yB; }
    bool valid() const { return det != 0.0; }

    void pixelToWorld(double px, double py, double& X, double& Y) const {
        X = xA + xB * px + xC * py;
        Y = yA + yB * px + yC * py;
    }
    void worldToPixel(double X, double Y, double& px, double& py) const {
        const double dx = X - xA, dy = Y - yA;
        px = ( yC * dx - xC * dy) / det;
        py = (-yB * dx + xB * dy) / det;
    }
    // Mercator metres spanned by one raster pixel (geometric mean of the two axis
    // scales); for a conformal Mercator chart the axes are equal, so this is just
    // the pixel size. Drives nativeZoom().
    double metresPerPixel() const { return std::sqrt(std::abs(det)); }
};

// One parsed chart: immutable after parse(), so renderTile() is const and safe
// to call concurrently from several threads (it opens its own file handle).
class KapChart {
public:
    // Parse the header and index table of `path`. Does not decode any imagery.
    // Returns false + err for unreadable files, and for charts this reader
    // deliberately declines (a non-Mercator projection — see parse()) — the
    // caller should skip those rather than draw them in the wrong place. Skewed
    // (rotated) Mercator charts are supported, not declined.
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
    // Decode source row `row` into `out` (palette indices, width_ entries), but
    // stop once column `maxCol` is reached — a tile only needs the columns up to
    // its right edge, and RLE rows must be decoded from the start, so this caps
    // the wasted work at the tail. `f` is a file handle owned by the caller (one
    // per renderTile call).
    bool readRow(class QFile& f, int row, std::vector<uint8_t>& out, int maxCol) const;

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
