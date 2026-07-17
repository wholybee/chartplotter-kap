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

// Least-squares fit of v = a + b*u over the control points. Returns false when
// the u values are (near) coincident, i.e. the points don't constrain the axis.
bool linearFit(const std::vector<double>& u, const std::vector<double>& v,
               double& a, double& b) {
    const int n = static_cast<int>(u.size());
    if (n < 2) return false;
    double su = 0, sv = 0, suu = 0, suv = 0;
    for (int i = 0; i < n; ++i) {
        su += u[i]; sv += v[i];
        suu += u[i] * u[i]; suv += u[i] * v[i];
    }
    const double denom = n * suu - su * su;
    if (std::abs(denom) < 1e-9) return false;
    b = (n * suv - su * sv) / denom;
    a = (sv - b * su) / n;
    return b != 0.0;
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
    std::vector<double> refPx, refPy, refLon, refMerc;
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
                refLon.push_back(p[4].toDouble());
                refMerc.push_back(proj::latToY(p[3].toDouble()));
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
    // This reader models a north-up Mercator chart and nothing else. A polyconic
    // or skewed chart fitted with that model lands in visibly the wrong place,
    // and a chart drawn in the wrong place is worse than a chart not drawn — so
    // decline it and let the caller report it.
    const QString pr = knp.value(QStringLiteral("PR")).toUpper();
    if (!pr.isEmpty() && !pr.startsWith(QLatin1String("MERCATOR"))) {
        err = QStringLiteral("%1: projection %2 is not supported (Mercator only)")
                  .arg(QFileInfo(path).fileName(), pr);
        return false;
    }
    const double skew = knp.value(QStringLiteral("SK"), QStringLiteral("0")).toDouble();
    if (std::abs(skew) > 0.01) {
        err = QStringLiteral("%1: skewed charts are not supported (SK=%2)")
                  .arg(QFileInfo(path).fileName()).arg(skew);
        return false;
    }

    // --- georeference --------------------------------------------------------
    if (!linearFit(refPx, refLon, out.georef_.lonA, out.georef_.lonB) ||
        !linearFit(refPy, refMerc, out.georef_.mercC, out.georef_.mercD)) {
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
            const double px = out.georef_.lonToPx(ll.x());
            const double py = out.georef_.mercYToPy(proj::latToY(ll.y()));
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
    // At zoom z the world is 256*2^z px across 360 degrees. The chart is width_
    // px across its own longitude span. Equating the two pixel scales:
    //   256*2^z / 360 = width_ / lonSpan   =>   z = log2(360*width_ / (256*lonSpan))
    const double lonSpan = std::abs(georef_.lonB) * width_;
    if (lonSpan <= 0.0) return 0;
    const double z = std::log2(360.0 * width_ / (256.0 * lonSpan));
    return std::clamp(static_cast<int>(std::lround(z)), 0, 22);
}

void KapChart::lonLatBounds(double& minLon, double& minLat,
                            double& maxLon, double& maxLat) const {
    const double lon0 = georef_.pxToLon(0), lon1 = georef_.pxToLon(width_);
    const double lat0 = proj::yToLat(georef_.pyToMercY(0));
    const double lat1 = proj::yToLat(georef_.pyToMercY(height_));
    minLon = std::min(lon0, lon1);  maxLon = std::max(lon0, lon1);
    minLat = std::min(lat0, lat1);  maxLat = std::max(lat0, lat1);
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
bool KapChart::readRow(QFile& f, int row, std::vector<uint8_t>& out) const {
    out.assign(width_, 0);
    if (row < 0 || row >= height_) return false;

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
    while (i < n && px < width_) {
        uchar c = p[i++];
        if (c == 0) break;                        // end of row
        const int colour = (c & 0x7f) >> colourShift;
        int count = c & countMask;
        while ((c & 0x80) && i < n) {
            c = p[i++];
            count = (count << 7) + (c & 0x7f);
        }
        ++count;
        count = std::min(count, width_ - px);
        std::memset(out.data() + px, colour, count);
        px += count;
    }
    // Some encoders drop the trailing run when it reaches the row's end. Extend
    // the last colour rather than leaving a stripe of index 0.
    if (px > 0 && px < width_)
        std::memset(out.data() + px, out[px - 1], width_ - px);
    return true;
}

bool KapChart::renderTile(int z, int x, int y, int tilePx,
                          QImage& img, QString& err) const {
    if (tilePx <= 0 || !georef_.valid()) return false;

    // Tile bounds -> source pixel bounds. Both mappings are linear, so the tile's
    // pixel rect is exact rather than a search.
    const double lonL  = tileToLon(x,     z);
    const double lonR  = tileToLon(x + 1, z);
    const double mercT = tileToMercY(y,     z);
    const double mercB = tileToMercY(y + 1, z);

    const double pxL = georef_.lonToPx(lonL),   pxR = georef_.lonToPx(lonR);
    const double pyT = georef_.mercYToPy(mercT), pyB = georef_.mercYToPy(mercB);

    // Reject tiles that miss the chart entirely (the host caches this as absent).
    if (std::max(pxL, pxR) <= 0 || std::min(pxL, pxR) >= width_ ||
        std::max(pyT, pyB) <= 0 || std::min(pyT, pyB) >= height_) {
        img = QImage();
        return true;
    }

    // Downscale factor per axis: how many source pixels fall in one tile pixel.
    // Box-average that many samples so zoomed-out charts stay legible instead of
    // dissolving into nearest-neighbour aliasing on thin linework and text — but
    // cap the sample count, since the cost of a tile is (row samples) x (row
    // decodes) and a 32x downscale would otherwise decode 8192 rows.
    constexpr int kMaxSamples = 4;
    const double spanX = std::abs(pxR - pxL) / tilePx;
    const double spanY = std::abs(pyB - pyT) / tilePx;
    const int nsx = std::clamp(static_cast<int>(std::lround(spanX)), 1, kMaxSamples);
    const int nsy = std::clamp(static_cast<int>(std::lround(spanY)), 1, kMaxSamples);

    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot open %1: %2").arg(path_, f.errorString());
        return false;
    }

    img = QImage(tilePx, tilePx, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    // Column sample table: for each destination column, the source columns to
    // average. Built once and reused for every row of the tile.
    std::vector<std::array<int, kMaxSamples>> colSamples(tilePx);
    std::vector<int> colCount(tilePx, 0);
    for (int i = 0; i < tilePx; ++i) {
        for (int s = 0; s < nsx; ++s) {
            const double t  = (i + (s + 0.5) / nsx) / tilePx;
            const double sx = pxL + (pxR - pxL) * t;
            const int    cx = static_cast<int>(std::floor(sx));
            if (cx < 0 || cx >= width_) continue;
            colSamples[i][colCount[i]++] = cx;
        }
    }

    std::vector<uint8_t> rowBuf;
    // Accumulators for one destination row.
    std::vector<int> accR(tilePx), accG(tilePx), accB(tilePx), accA(tilePx), accN(tilePx);

    bool any = false;
    for (int j = 0; j < tilePx; ++j) {
        std::fill(accR.begin(), accR.end(), 0);
        std::fill(accG.begin(), accG.end(), 0);
        std::fill(accB.begin(), accB.end(), 0);
        std::fill(accA.begin(), accA.end(), 0);
        std::fill(accN.begin(), accN.end(), 0);

        for (int s = 0; s < nsy; ++s) {
            const double t  = (j + (s + 0.5) / nsy) / tilePx;
            const double sy = pyT + (pyB - pyT) * t;
            const int    ry = static_cast<int>(std::floor(sy));
            if (ry < 0 || ry >= height_) continue;
            if (!readRow(f, ry, rowBuf)) continue;

            for (int i = 0; i < tilePx; ++i) {
                for (int c = 0; c < colCount[i]; ++c) {
                    const int cx = colSamples[i][c];
                    // Outside the chart's real coverage (paper collar / legend on
                    // a scanned chart): contribute nothing, so neighbours quilt
                    // without a border overlapping them.
                    if (!plyPixels_.empty() &&
                        !pointInPolygon(plyPixels_, cx + 0.5, ry + 0.5))
                        continue;
                    const QRgb v = palette_[rowBuf[cx]];
                    accR[i] += qRed(v); accG[i] += qGreen(v);
                    accB[i] += qBlue(v); accA[i] += qAlpha(v);
                    ++accN[i];
                }
            }
        }

        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(j));
        for (int i = 0; i < tilePx; ++i) {
            if (!accN[i]) continue;                 // stays transparent
            const int n = accN[i];
            line[i] = qRgba(accR[i] / n, accG[i] / n, accB[i] / n, accA[i] / n);
            any = true;
        }
    }

    // Nothing landed on the tile after coverage clipping — report it empty so the
    // host stops asking for it.
    if (!any) img = QImage();
    return true;
}
