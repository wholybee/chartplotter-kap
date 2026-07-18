// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart reader for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#include "kap_reader.hpp"

#include "projection.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

// Largest header we'll scan for the 0x1A separator. Real headers run a few KB
// (mostly the RGB palette); this bounds the damage from a non-KAP file that
// happens to have a .kap extension.
constexpr qint64 kMaxHeaderBytes = 1 << 20;   // 1 MiB

// ---- header text ------------------------------------------------------------

// One header entry: the 3-letter token and its raw value text.
// Lines starting with whitespace continue the previous entry.
QList<QPair<QString, QString>> splitEntries(const QString& text) {
    QList<QPair<QString, QString>> out;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    QString cur;
    auto flush = [&out, &cur]() {
        if (cur.isEmpty()) return;
        const int slash = cur.indexOf(QLatin1Char('/'));
        if (slash > 0)
            out.append({cur.left(slash).trimmed(), cur.mid(slash + 1).trimmed()});
        cur.clear();
    };
    for (const QString& raw : lines) {
        if (raw.trimmed().isEmpty()) continue;
        if (raw.at(0).isSpace()) {
            // Continuation: fold onto the previous entry as another comma field.
            if (!cur.isEmpty()) cur += QLatin1Char(',') + raw.trimmed();
        } else {
            flush();
            cur = raw;
        }
    }
    flush();
    return out;
}

// Parse a `k=v,k=v` value list.
//
// Quirk: values may themselves contain commas — `RA=2048,2048` is the canonical
// offender. A comma field with no '=' is therefore a continuation of the
// previous key's value, not a new key. Splitting naively silently truncates the
// raster height to nothing, so this is load-bearing.
QHash<QString, QString> parseKv(const QString& value) {
    QHash<QString, QString> kv;
    QString lastKey;
    const QStringList parts = value.split(QLatin1Char(','));
    for (const QString& p : parts) {
        const int eq = p.indexOf(QLatin1Char('='));
        if (eq > 0) {
            lastKey = p.left(eq).trimmed();
            kv.insert(lastKey, p.mid(eq + 1).trimmed());
        } else if (!lastKey.isEmpty()) {
            kv[lastKey] += QLatin1Char(',') + p.trimmed();
        }
    }
    return kv;
}

// Least-squares fit of the affine map (px,py) -> (X,Y):
//   X = xA + xB*px + xC*py,   Y = yA + yB*px + yC*py
// over the REF control points.
//
// Solved on coordinates centred at their means: centring decouples the constant
// term (it becomes the mean of X / Y) and leaves a well-conditioned 2x2 covariance
// system for the two slopes — pixel coordinates run into the tens of thousands, so
// fitting them raw would lose precision and make a sane singularity test hard.
// Returns false when the control points are collinear (they don't pin a 2-D
// transform) or the resulting transform is degenerate.
bool affineFit(const std::vector<double>& px, const std::vector<double>& py,
               const std::vector<double>& X, const std::vector<double>& Y,
               KapGeoref& g) {
    const int n = static_cast<int>(px.size());
    if (n < 3) return false;

    double mpx = 0, mpy = 0, mX = 0, mY = 0;
    for (int i = 0; i < n; ++i) { mpx += px[i]; mpy += py[i]; mX += X[i]; mY += Y[i]; }
    mpx /= n; mpy /= n; mX /= n; mY /= n;

    // Centred moments: Suu = Σ dpx², Svv = Σ dpy², Suv = Σ dpx·dpy, and the
    // cross-products of each world axis with the centred pixel coordinates.
    double Suu = 0, Svv = 0, Suv = 0;
    double SuX = 0, SvX = 0, SuY = 0, SvY = 0;
    for (int i = 0; i < n; ++i) {
        const double u = px[i] - mpx, v = py[i] - mpy;
        const double dX = X[i] - mX,  dY = Y[i] - mY;
        Suu += u * u; Svv += v * v; Suv += u * v;
        SuX += u * dX; SvX += v * dX;
        SuY += u * dY; SvY += v * dY;
    }
    // Collinear control points give a rank-deficient covariance matrix; reject
    // relative to its scale so the test is independent of chart size.
    const double det2 = Suu * Svv - Suv * Suv;
    if (det2 <= 1e-12 * Suu * Svv) return false;

    // Slopes from the 2x2 solve, one right-hand side per world axis.
    g.xB = ( Svv * SuX - Suv * SvX) / det2;
    g.xC = (-Suv * SuX + Suu * SvX) / det2;
    g.yB = ( Svv * SuY - Suv * SvY) / det2;
    g.yC = (-Suv * SuY + Suu * SvY) / det2;
    // Fold the centring offset back into the constant term.
    g.xA = mX - g.xB * mpx - g.xC * mpy;
    g.yA = mY - g.yB * mpx - g.yC * mpy;
    g.finalize();
    return g.valid();
}

// XYZ tile edges in Web Mercator. x maps to longitude linearly; y maps to
// projected metres linearly (which is what makes a Mercator KAP a linear fit).
double tileToLon(double x, int z) {
    return x / std::pow(2.0, z) * 360.0 - 180.0;
}
double tileToMercY(double y, int z) {
    // Tile row 0 is the north edge; the projected span is symmetric about 0.
    const double worldHalf = proj::PI * proj::kEarthRadius;
    return worldHalf - (y / std::pow(2.0, z)) * (2.0 * worldHalf);
}

// Even-odd point-in-polygon over a pixel-space ring.
bool pointInPolygon(const std::vector<QPointF>& poly, double x, double y) {
    bool in = false;
    const int n = static_cast<int>(poly.size());
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const double xi = poly[i].x(), yi = poly[i].y();
        const double xj = poly[j].x(), yj = poly[j].y();
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

}  // namespace

// ---- parsing ----------------------------------------------------------------

bool KapChart::parse(const QString& path, KapChart& out, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
        return false;
    }
    const qint64 fileSize = f.size();
    if (fileSize < 16) {
        err = QStringLiteral("%1: too small to be a KAP").arg(path);
        return false;
    }

    // --- header text, up to the 0x1A separator -------------------------------
    QByteArray head = f.read(std::min<qint64>(fileSize, kMaxHeaderBytes));
    const int sep = head.indexOf('\x1a');
    if (sep < 0) {
        err = QStringLiteral("%1: no header/raster separator (not a KAP?)").arg(path);
        return false;
    }
    // The raster starts after the 0x1A and the single 0x00 that conventionally
    // follows it. Skip EXACTLY one NUL, never a run of them: a single-colour
    // chart has depth 0, so its depth byte is itself 0x00 and a greedy skip
    // swallows it (and then mis-reads the first run byte as the depth).
    qint64 rasterStart = sep + 1;
    if (rasterStart < head.size() && head.at(rasterStart) == '\0') ++rasterStart;

    // Latin-1: headers are ASCII in practice, and it never throws away bytes.
    const QString text = QString::fromLatin1(head.constData(), sep);
    const auto entries = splitEntries(text);

    QHash<QString, QString> bsb, knp;
    // REF control points as pixel (px,py) -> Mercator metres (X,Y).
    std::vector<double> refPx, refPy, refX, refY;
    std::vector<QPointF> plyLonLat;
    int ifm = 0;
    out.palette_.assign(256, qRgb(0, 0, 0));
    std::vector<bool> palSet(256, false);

    for (const auto& [tok, val] : entries) {
        if (tok == QLatin1String("BSB") || tok == QLatin1String("NOS")) {
            bsb = parseKv(val);
        } else if (tok == QLatin1String("KNP")) {
            knp = parseKv(val);
        } else if (tok == QLatin1String("IFM")) {
            ifm = val.trimmed().toInt();
        } else if (tok == QLatin1String("RGB")) {
            // RGB/<index>,<r>,<g>,<b>
            const QStringList p = val.split(QLatin1Char(','));
            if (p.size() >= 4) {
                const int i = p[0].toInt();
                if (i >= 0 && i < 256) {
                    out.palette_[i] = qRgb(p[1].toInt(), p[2].toInt(), p[3].toInt());
                    palSet[i] = true;
                }
            }
        } else if (tok == QLatin1String("REF")) {
            // REF/<n>,<px>,<py>,<lat>,<lon>
            const QStringList p = val.split(QLatin1Char(','));
            if (p.size() >= 5) {
                refPx.push_back(p[1].toDouble());
                refPy.push_back(p[2].toDouble());
                refX.push_back(proj::lonToX(p[4].toDouble()));
                refY.push_back(proj::latToY(p[3].toDouble()));
            }
        } else if (tok == QLatin1String("PLY")) {
            // PLY/<n>,<lat>,<lon>
            const QStringList p = val.split(QLatin1Char(','));
            if (p.size() >= 3)
                plyLonLat.push_back(QPointF(p[2].toDouble(), p[1].toDouble()));
        }
    }

    // --- raster dimensions ---------------------------------------------------
    const QStringList ra = bsb.value(QStringLiteral("RA")).split(QLatin1Char(','));
    if (ra.size() < 2) {
        err = QStringLiteral("%1: missing BSB/RA raster size").arg(path);
        return false;
    }
    out.width_  = ra[0].trimmed().toInt();
    out.height_ = ra[1].trimmed().toInt();
    if (out.width_ <= 0 || out.height_ <= 0) {
        err = QStringLiteral("%1: bad raster size %2x%3")
                  .arg(path).arg(out.width_).arg(out.height_);
        return false;
    }

    out.path_  = path;
    out.name_  = bsb.value(QStringLiteral("NA"));
    if (out.name_.isEmpty()) out.name_ = QFileInfo(path).completeBaseName();
    out.scale_ = knp.value(QStringLiteral("SC")).toDouble();

    // --- projection gate -----------------------------------------------------
    // This reader models a Mercator chart — including skewed (rotated) ones, which
    // the affine georeference below handles. A genuinely different projection
    // (polyconic, transverse Mercator, ...) fitted with a Mercator affine lands in
    // visibly the wrong place, and a chart drawn in the wrong place is worse than a
    // chart not drawn — so decline those and let the caller report them.
    const QString pr = knp.value(QStringLiteral("PR")).toUpper();
    if (!pr.isEmpty() && !pr.startsWith(QLatin1String("MERCATOR"))) {
        err = QStringLiteral("%1: projection %2 is not supported (Mercator only)")
                  .arg(QFileInfo(path).fileName(), pr);
        return false;
    }

    // --- georeference --------------------------------------------------------
    // One affine fit over the REF points, covering north-up and skewed charts
    // alike (a north-up chart simply comes back with near-zero cross terms).
    if (!affineFit(refPx, refPy, refX, refY, out.georef_)) {
        err = QStringLiteral("%1: REF points do not define a Mercator georeference")
                  .arg(QFileInfo(path).fileName());
        return false;
    }

    // --- palette -------------------------------------------------------------
    // BSB palettes are 1-based; index 0 is conventionally unused. Leave any
    // index the file never defined fully transparent so a stray value shows the
    // basemap through rather than painting a black hole in the chart.
    for (int i = 0; i < 256; ++i)
        if (!palSet[i]) out.palette_[i] = qRgba(0, 0, 0, 0);

    // --- raster depth --------------------------------------------------------
    if (rasterStart >= fileSize) {
        err = QStringLiteral("%1: header runs past end of file").arg(path);
        return false;
    }
    f.seek(rasterStart);
    char depthByte = 0;
    if (f.read(&depthByte, 1) != 1) {
        err = QStringLiteral("%1: truncated before raster depth").arg(path);
        return false;
    }
    // The depth byte is authoritative for decoding; IFM is the header's claim.
    // They agree on every well-formed chart — prefer the byte, fall back to IFM
    // only if the byte is implausible.
    //
    // Depth 0 is legal, not a defect: a chart of one solid colour (open ocean,
    // in a tiled set) carries a single palette entry and encodes every row as
    // runs of colour 0, so imgkap writes IFM/0. The run decoder handles it
    // without a special case — see readRow.
    out.depth_ = static_cast<unsigned char>(depthByte);
    if (out.depth_ > 7) out.depth_ = ifm;
    if (out.depth_ < 0 || out.depth_ > 7) {
        err = QStringLiteral("%1: unsupported raster depth %2").arg(path).arg(out.depth_);
        return false;
    }

    // --- row index table -----------------------------------------------------
    // Last 4 bytes point at a table of `height` big-endian row offsets.
    f.seek(fileSize - 4);
    QByteArray tail = f.read(4);
    if (tail.size() != 4) {
        err = QStringLiteral("%1: truncated index pointer").arg(path);
        return false;
    }
    const quint32 idxOff = (quint32(quint8(tail[0])) << 24) | (quint32(quint8(tail[1])) << 16) |
                           (quint32(quint8(tail[2])) << 8)  |  quint32(quint8(tail[3]));
    const qint64 need = qint64(out.height_) * 4;
    if (qint64(idxOff) < rasterStart || qint64(idxOff) + need > fileSize - 4) {
        err = QStringLiteral("%1: index table offset %2 out of range").arg(path).arg(idxOff);
        return false;
    }
    f.seek(idxOff);
    QByteArray idx = f.read(need);
    if (idx.size() != need) {
        err = QStringLiteral("%1: truncated index table").arg(path);
        return false;
    }
    out.rowOffsets_.resize(out.height_ + 1);
    for (int i = 0; i < out.height_; ++i) {
        const uchar* p = reinterpret_cast<const uchar*>(idx.constData()) + i * 4;
        out.rowOffsets_[i] = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) |
                             (quint32(p[2]) << 8)  |  quint32(p[3]);
    }
    // Sentinel: the table itself bounds the last row, so every row has an end.
    out.rowOffsets_[out.height_] = idxOff;

    // Offsets must be inside the raster and non-decreasing, or the file is not
    // the KAP its extension claims and we'd be decoding noise.
    for (int i = 0; i < out.height_; ++i) {
        if (out.rowOffsets_[i] < rasterStart ||
            out.rowOffsets_[i] > out.rowOffsets_[i + 1]) {
            err = QStringLiteral("%1: row index table is not monotonic at row %2")
                      .arg(path).arg(i);
            return false;
        }
    }

    // --- coverage polygon ----------------------------------------------------
    // Convert PLY to pixel space so renderTile can test the source pixel it is
    // already computing. Drop it when it covers the whole image (tiled mosaics
    // like the OSM-derived sets), since then every test would pass anyway.
    if (plyLonLat.size() >= 3) {
        bool coversAll = true;
        out.plyPixels_.reserve(plyLonLat.size());
        for (const QPointF& ll : plyLonLat) {
            double px, py;
            out.georef_.worldToPixel(proj::lonToX(ll.x()), proj::latToY(ll.y()), px, py);
            out.plyPixels_.push_back(QPointF(px, py));
            // "Whole image" = every vertex sits on the image corner grid, within
            // a pixel of tolerance.
            const bool onX = std::abs(px) <= 1.0 || std::abs(px - out.width_)  <= 1.0;
            const bool onY = std::abs(py) <= 1.0 || std::abs(py - out.height_) <= 1.0;
            if (!onX || !onY) coversAll = false;
        }
        if (coversAll) out.plyPixels_.clear();
    }

    return true;
}

// ---- geometry ---------------------------------------------------------------

int KapChart::nativeZoom() const {
    // At zoom z the whole Mercator world (2*pi*R metres) is 256*2^z tile pixels
    // across, so a tile pixel spans ww/(256*2^z) metres. The chart's own pixel
    // spans metresPerPixel() metres. Equating the two scales:
    //   ww/(256*2^z) = metresPerPixel   =>   z = log2(ww / (256*metresPerPixel))
    const double mpp = georef_.metresPerPixel();
    if (mpp <= 0.0) return 0;
    const double ww = 2.0 * proj::PI * proj::kEarthRadius;
    const double z = std::log2(ww / (256.0 * mpp));
    return std::clamp(static_cast<int>(std::lround(z)), 0, 22);
}

void KapChart::lonLatBounds(double& minLon, double& minLat,
                            double& maxLon, double& maxLat) const {
    // A skewed chart's raster corners aren't its lon/lat extremes along either
    // axis, so map all four corners and take the envelope.
    minLon = minLat = 1e30;
    maxLon = maxLat = -1e30;
    const double corners[4][2] = {{0, 0},
                                  {double(width_), 0},
                                  {double(width_), double(height_)},
                                  {0, double(height_)}};
    for (const auto& c : corners) {
        double X, Y;
        georef_.pixelToWorld(c[0], c[1], X, Y);
        const double lon = proj::xToLon(X), lat = proj::yToLat(Y);
        minLon = std::min(minLon, lon);  maxLon = std::max(maxLon, lon);
        minLat = std::min(minLat, lat);  maxLat = std::max(maxLat, lat);
    }
}

// ---- raster -----------------------------------------------------------------

// Decode one RLE row into palette indices.
//
// Row layout: a variable-length row number (7 bits per byte, high bit =
// "another byte follows"), then runs until a 0x00 terminator. Each run's first
// byte packs the colour in its top `depth` bits (below the continuation bit) and
// the low bits start the count, which further continuation bytes extend 7 bits
// at a time. Note that at depth 7 the colour uses all 7 bits, so the count comes
// entirely from continuation bytes — the shifts below handle that naturally.
bool KapChart::readRow(QFile& f, int row, std::vector<uint8_t>& out, int maxCol) const {
    out.assign(width_, 0);
    if (row < 0 || row >= height_) return false;

    // Decode no further than the caller needs. Columns past `lim` stay 0 and are
    // never sampled, so we can stop the run loop as soon as we cover `lim`.
    const int lim = std::clamp(maxCol, 1, width_);

    const quint32 start = rowOffsets_[row];
    const quint32 end   = rowOffsets_[row + 1];
    if (end <= start) return false;
    if (!f.seek(start)) return false;

    const QByteArray buf = f.read(end - start);
    if (buf.isEmpty()) return false;

    const uchar* p = reinterpret_cast<const uchar*>(buf.constData());
    const int n = buf.size();
    int i = 0;

    // Row number: skip continuation bytes, then the terminating one.
    while (i < n && (p[i] & 0x80)) ++i;
    ++i;

    const int colourShift = 7 - depth_;
    const int countMask   = 0x7f >> depth_;

    int px = 0;
    while (i < n && px < lim) {
        uchar c = p[i++];
        if (c == 0) break;                        // end of row
        const int colour = (c & 0x7f) >> colourShift;
        int count = c & countMask;
        while ((c & 0x80) && i < n) {
            c = p[i++];
            count = (count << 7) + (c & 0x7f);
        }
        ++count;
        count = std::min(count, lim - px);
        std::memset(out.data() + px, colour, count);
        px += count;
    }
    // Some encoders drop the trailing run when it reaches the row's end. Extend
    // the last colour rather than leaving a stripe of index 0 (only up to `lim`).
    if (px > 0 && px < lim)
        std::memset(out.data() + px, out[px - 1], lim - px);
    return true;
}

bool KapChart::renderTile(int z, int x, int y, int tilePx,
                          QImage& img, QString& err) const {
    if (tilePx <= 0 || !georef_.valid()) return false;

    // Tile edges in Mercator metres (X linear in lon, Y is mercY). A tile is an
    // axis-aligned rectangle in this world frame; on a skewed chart it maps to a
    // rotated quad in raster pixels, so we can no longer treat the pixel footprint
    // as an axis-aligned rect and sample columns/rows separably.
    const double Xl = proj::lonToX(tileToLon(x,     z));
    const double Xr = proj::lonToX(tileToLon(x + 1, z));
    const double Yt = tileToMercY(y,     z);
    const double Yb = tileToMercY(y + 1, z);

    // Map the four tile corners into raster pixel space and take their bounding
    // box: it tells us whether the tile touches the chart at all, how far to
    // downsample, and which source rows we might read.
    const double cornersXY[4][2] = {{Xl, Yt}, {Xr, Yt}, {Xr, Yb}, {Xl, Yb}};
    double pxMin = 1e30, pxMax = -1e30, pyMin = 1e30, pyMax = -1e30;
    for (const auto& c : cornersXY) {
        double px, py;
        georef_.worldToPixel(c[0], c[1], px, py);
        pxMin = std::min(pxMin, px); pxMax = std::max(pxMax, px);
        pyMin = std::min(pyMin, py); pyMax = std::max(pyMax, py);
    }

    // Reject tiles that miss the chart entirely (the host caches this as absent).
    if (pxMax <= 0 || pxMin >= width_ || pyMax <= 0 || pyMin >= height_) {
        img = QImage();
        return true;
    }

    // Downscale factor per axis: how many source pixels fall in one tile pixel,
    // read off the footprint's raster extent. Box-average that many sub-samples so
    // zoomed-out charts stay legible instead of aliasing thin linework and text.
    // The two axes are capped differently on purpose: horizontal sub-samples are
    // just extra lookups into a row that's already decoded and in memory, but each
    // vertical sub-sample pulls in another source row to decode — the dominant tile
    // cost — so we spend generously across and sparingly down.
    constexpr int kMaxSamplesX = 4;
    constexpr int kMaxSamplesY = 2;
    const int nsx = std::clamp(static_cast<int>(std::lround((pxMax - pxMin) / tilePx)),
                               1, kMaxSamplesX);
    const int nsy = std::clamp(static_cast<int>(std::lround((pyMax - pyMin) / tilePx)),
                               1, kMaxSamplesY);

    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot open %1: %2").arg(path_, f.errorString());
        return false;
    }

    img = QImage(tilePx, tilePx, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    // Decode source rows lazily and once each: the footprint's rotated quad can
    // touch any row in [r0, r1], and neighbouring sub-samples hit the same rows
    // repeatedly. Rows outside the chart never allocate.
    const int r0 = std::max(0, static_cast<int>(std::floor(pyMin)));
    const int r1 = std::min(height_ - 1, static_cast<int>(std::ceil(pyMax)));
    if (r1 < r0) { img = QImage(); return true; }
    const int maxCol = std::clamp(static_cast<int>(std::ceil(pxMax)) + 1, 1, width_);
    std::vector<std::vector<uint8_t>> rows(r1 - r0 + 1);
    std::vector<char> loaded(r1 - r0 + 1, 0);
    auto rowFor = [&](int ry) -> const std::vector<uint8_t>* {
        if (ry < r0 || ry > r1) return nullptr;
        const int k = ry - r0;
        if (!loaded[k]) {
            loaded[k] = 1;
            if (!readRow(f, ry, rows[k], maxCol)) rows[k].clear();
        }
        return rows[k].empty() ? nullptr : &rows[k];
    };

    const bool havePly = !plyPixels_.empty();
    bool any = false;
    for (int j = 0; j < tilePx; ++j) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(j));
        for (int i = 0; i < tilePx; ++i) {
            // Coverage test once per output pixel (at its centre), not per
            // sub-sample: the paper collar / legend on a scanned chart is clipped
            // so neighbours quilt without an overlapping border, and testing the
            // polygon 16x per pixel was the dominant cost when downsampling. The
            // resulting collar edge is hard to within one output pixel, which is
            // exactly where charts abut anyway.
            if (havePly) {
                double px, py;
                georef_.worldToPixel(Xl + (Xr - Xl) * ((i + 0.5) / tilePx),
                                     Yt + (Yb - Yt) * ((j + 0.5) / tilePx), px, py);
                if (!pointInPolygon(plyPixels_, px, py)) continue;
            }

            int aR = 0, aG = 0, aB = 0, aA = 0, aN = 0;
            for (int sy = 0; sy < nsy; ++sy) {
                const double Y = Yt + (Yb - Yt) * ((j + (sy + 0.5) / nsy) / tilePx);
                for (int sx = 0; sx < nsx; ++sx) {
                    const double X = Xl + (Xr - Xl) * ((i + (sx + 0.5) / nsx) / tilePx);

                    double px, py;
                    georef_.worldToPixel(X, Y, px, py);
                    const int cx = static_cast<int>(std::floor(px));
                    const int ry = static_cast<int>(std::floor(py));
                    if (cx < 0 || cx >= width_ || ry < 0 || ry >= height_) continue;
                    const std::vector<uint8_t>* row = rowFor(ry);
                    if (!row) continue;
                    const QRgb v = palette_[(*row)[cx]];
                    aR += qRed(v); aG += qGreen(v);
                    aB += qBlue(v); aA += qAlpha(v);
                    ++aN;
                }
            }
            if (aN) {
                line[i] = qRgba(aR / aN, aG / aN, aB / aN, aA / aN);
                any = true;
            }
        }
    }

    // Nothing landed on the tile after coverage clipping — report it empty so the
    // host stops asking for it.
    if (!any) img = QImage();
    return true;
}
