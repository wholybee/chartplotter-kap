// SPDX-License-Identifier: GPL-2.0-or-later
//
// Standalone smoke test for the BSB/KAP reader — no GUI, no host.
//
//   test_kap <KAP folder> [outdir]
//
// Parses every *.kap under the folder, checks the georeference round-trips, and
// renders tiles through exactly the path the plugin uses at runtime. It writes
// two PNGs to `outdir` (default: the working directory):
//
//   kap_mosaic.png  every chart composited into one image by geography. This is
//                   the real test: the charts only line up into a coherent
//                   coastline if each one's georeference is right, so a correct
//                   run is recognisable at a glance and a broken one is obvious.
//   kap_native.png  one tile at native zoom, to eyeball decode quality 1:1.
//
// Exits non-zero if any check fails.
#include "kap_reader.hpp"
#include "projection.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <vector>

static QTextStream out(stdout);
static int failures = 0;

static void check(bool ok, const QString& what) {
    if (!ok) {
        out << "  FAIL: " << what << Qt::endl;
        ++failures;
    }
}

// Mirror of the reader's tile maths, so the test computes tile geography
// independently rather than trusting the same helper it is checking.
static double tileToLon(double x, int z) {
    return x / std::pow(2.0, z) * 360.0 - 180.0;
}
static double lonToTile(double lon, int z) {
    return (lon + 180.0) / 360.0 * std::pow(2.0, z);
}
static double latToTile(double lat, int z) {
    const double r = lat * proj::kDeg2Rad;
    return (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / proj::PI) / 2.0 *
           std::pow(2.0, z);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        out << "usage: test_kap <KAP folder> [outdir]" << Qt::endl;
        return 2;
    }
    const QString root = QString::fromLocal8Bit(argv[1]);
    const QString outDir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QDir::currentPath();

    QStringList paths;
    QDirIterator it(root, {QStringLiteral("*.kap"), QStringLiteral("*.KAP")},
                    QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) paths << it.next();
    paths.sort();
    if (paths.isEmpty()) {
        out << "no *.kap found under " << root << Qt::endl;
        return 2;
    }
    out << "found " << paths.size() << " KAP file(s) under " << root << Qt::endl;

    QElapsedTimer timer;
    timer.start();

    std::vector<KapChart> charts;
    charts.reserve(paths.size());
    int skipped = 0;

    for (const QString& p : paths) {
        KapChart c;
        QString err;
        if (!KapChart::parse(p, c, err)) {
            out << "  skip: " << err << Qt::endl;
            ++skipped;
            continue;
        }
        charts.push_back(std::move(c));
    }
    out << "parsed " << charts.size() << " chart(s), skipped " << skipped
        << " in " << timer.elapsed() << " ms" << Qt::endl;
    check(!charts.empty(), QStringLiteral("no charts parsed"));
    if (charts.empty()) return 1;

    // ---- per-chart checks ---------------------------------------------------
    double gMinLon = 1e30, gMinLat = 1e30, gMaxLon = -1e30, gMaxLat = -1e30;
    for (const KapChart& c : charts) {
        double minLon, minLat, maxLon, maxLat;
        c.lonLatBounds(minLon, minLat, maxLon, maxLat);
        gMinLon = std::min(gMinLon, minLon); gMaxLon = std::max(gMaxLon, maxLon);
        gMinLat = std::min(gMinLat, minLat); gMaxLat = std::max(gMaxLat, maxLat);

        const KapGeoref& g = c.georef();
        check(g.valid(), c.name() + QStringLiteral(": georeference not valid"));

        // Round-trip: pixel -> lon -> pixel must land back on the same pixel.
        const double rt = g.lonToPx(g.pxToLon(c.width() / 2.0));
        check(std::abs(rt - c.width() / 2.0) < 1e-6,
              c.name() + QStringLiteral(": lon round-trip drifted"));

        check(c.width() > 0 && c.height() > 0,
              c.name() + QStringLiteral(": bad raster size"));
        // 0 is legal: a solid-colour chart (see KapChart::parse).
        check(c.depth() >= 0 && c.depth() <= 7,
              c.name() + QStringLiteral(": bad depth"));

        const int z = c.nativeZoom();
        check(z > 0 && z <= 22, c.name() + QStringLiteral(": implausible native zoom"));

        // At native zoom one chart pixel should equal one tile pixel. Check the
        // chart's width in tile-pixels matches its raster width.
        const double tw = (lonToTile(maxLon, z) - lonToTile(minLon, z)) * 256.0;
        check(std::abs(tw - c.width()) < 2.0,
              QStringLiteral("%1: native zoom %2 gives %3 tile px for a %4 px raster")
                  .arg(c.name()).arg(z).arg(tw, 0, 'f', 1).arg(c.width()));
    }
    out << QStringLiteral("coverage: lon %1..%2  lat %3..%4")
               .arg(gMinLon, 0, 'f', 4).arg(gMaxLon, 0, 'f', 4)
               .arg(gMinLat, 0, 'f', 4).arg(gMaxLat, 0, 'f', 4)
        << Qt::endl;

    // ---- one native-zoom tile ----------------------------------------------
    // Render from the most detailed chart in the set (largest native zoom =
    // harbour scale). Picking charts.front() instead tends to land on open ocean,
    // where a uniform blue square proves nothing about the decoder.
    {
        const KapChart& c = *std::max_element(
            charts.begin(), charts.end(),
            [](const KapChart& a, const KapChart& b) {
                return a.nativeZoom() < b.nativeZoom();
            });
        const int z = c.nativeZoom();
        double minLon, minLat, maxLon, maxLat;
        c.lonLatBounds(minLon, minLat, maxLon, maxLat);
        // A tile comfortably inside the chart.
        const int tx = static_cast<int>(std::floor(lonToTile(minLon, z))) + 1;
        const int ty = static_cast<int>(std::floor(latToTile(maxLat, z))) + 1;

        QImage img;
        QString err;
        timer.restart();
        const bool ok = c.renderTile(z, tx, ty, 256, img, err);
        check(ok, QStringLiteral("native tile render failed: %1").arg(err));
        check(!img.isNull(), QStringLiteral("native tile came back empty"));
        if (!img.isNull()) {
            const QString p = QDir(outDir).filePath(QStringLiteral("kap_native.png"));
            img.save(p);
            out << "native tile z=" << z << " x=" << tx << " y=" << ty
                << " rendered in " << timer.elapsed() << " ms -> " << p << Qt::endl;
        }
    }

    // ---- geographic mosaic of every chart -----------------------------------
    // Pick the zoom that puts the whole coverage in roughly 1200 px, then
    // composite each chart's tiles into one image at their true positions.
    int mz = 0;
    for (int z = 0; z <= 22; ++z) {
        const double w = (lonToTile(gMaxLon, z) - lonToTile(gMinLon, z)) * 256.0;
        if (w > 1200.0) break;
        mz = z;
    }
    const int tx0 = static_cast<int>(std::floor(lonToTile(gMinLon, mz)));
    const int tx1 = static_cast<int>(std::ceil (lonToTile(gMaxLon, mz)));
    const int ty0 = static_cast<int>(std::floor(latToTile(gMaxLat, mz)));
    const int ty1 = static_cast<int>(std::ceil (latToTile(gMinLat, mz)));

    QImage mosaic((tx1 - tx0) * 256, (ty1 - ty0) * 256, QImage::Format_ARGB32);
    mosaic.fill(QColor(30, 40, 60));      // backdrop: gaps stay obvious
    {
        QPainter p(&mosaic);
        timer.restart();
        int drawn = 0, empty = 0;
        for (const KapChart& c : charts) {
            for (int ty = ty0; ty < ty1; ++ty) {
                for (int tx = tx0; tx < tx1; ++tx) {
                    QImage img;
                    QString err;
                    if (!c.renderTile(mz, tx, ty, 256, img, err)) {
                        check(false, QStringLiteral("tile render failed: %1").arg(err));
                        continue;
                    }
                    if (img.isNull()) { ++empty; continue; }
                    p.drawImage(QPoint((tx - tx0) * 256, (ty - ty0) * 256), img);
                    ++drawn;
                }
            }
        }
        out << "mosaic z=" << mz << ": " << drawn << " tiles drawn, " << empty
            << " empty, in " << timer.elapsed() << " ms" << Qt::endl;
        check(drawn > 0, QStringLiteral("mosaic drew no tiles"));
    }
    const QString mp = QDir(outDir).filePath(QStringLiteral("kap_mosaic.png"));
    mosaic.save(mp);
    out << "mosaic -> " << mp << " (" << mosaic.width() << "x" << mosaic.height() << ")"
        << Qt::endl;

    out << (failures ? QStringLiteral("FAILED: %1 check(s)").arg(failures)
                     : QStringLiteral("all checks passed"))
        << Qt::endl;
    return failures ? 1 : 0;
}
