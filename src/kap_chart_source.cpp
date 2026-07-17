// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart source for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#include "kap_chart_source.hpp"

#include "projection.hpp"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <algorithm>

namespace {

// Tile edge the host composites at.
constexpr int kTilePx = 256;

// How far below native zoom a chart will serve tiles. Zoomed right out the host
// clamps its natural zoom into [minZoom, maxZoom], so a floor that is too high
// makes a distant view request hundreds of native-resolution tiles per chart;
// too low and each tile box-averages over a huge span. 8 levels (256x) is enough
// to render any single chart into about one tile at the far end.
constexpr int kZoomLevelsBelowNative = 8;

QStringList kapFilter() {
    // KAP in the wild is .kap or .KAP; QDir's filter is case-insensitive on
    // Windows but not on Linux, so list both.
    return {QStringLiteral("*.kap"), QStringLiteral("*.KAP")};
}

}  // namespace

KapChartSource::KapChartSource() = default;
KapChartSource::~KapChartSource() = default;

bool KapChartSource::canHandle(const QString& root) const {
    // Cheap signature check: stop at the first *.kap anywhere under root. The
    // host calls this for every registered source on every chart-set switch, so
    // it must not parse anything.
    QDirIterator it(root, kapFilter(), QDir::Files | QDir::Readable,
                    QDirIterator::Subdirectories);
    return it.hasNext();
}

bool KapChartSource::catalog(const QString& root, std::vector<RasterSourceChart>& out,
                             QString& errMsg,
                             const std::function<void(int done, int total)>& progress) {
    QStringList paths;
    {
        QDirIterator it(root, kapFilter(), QDir::Files | QDir::Readable,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) paths << it.next();
    }
    if (paths.isEmpty()) {
        errMsg = QStringLiteral("no KAP charts under %1").arg(root);
        return false;
    }
    paths.sort();   // stable chart order between runs

    QHash<QString, std::shared_ptr<const KapChart>> parsed;
    int skipped = 0;
    QString firstSkip;

    for (int i = 0; i < paths.size(); ++i) {
        progress(i, paths.size());

        auto chart = std::make_shared<KapChart>();
        QString err;
        if (!KapChart::parse(paths[i], *chart, err)) {
            // One unreadable or unsupported chart (e.g. a polyconic one) must not
            // sink the whole folder: skip it and carry on.
            if (skipped++ == 0) firstSkip = err;
            qWarning().noquote() << "KAP:" << err;
            continue;
        }

        RasterSourceChart rc;
        rc.id   = chart->path();
        rc.name = chart->name();
        rc.maxZoom = chart->nativeZoom();
        rc.minZoom = std::max(0, rc.maxZoom - kZoomLevelsBelowNative);

        // Coverage in the host's scene frame: x = lonToX, y = -latToY (north-up).
        double minLon, minLat, maxLon, maxLat;
        chart->lonLatBounds(minLon, minLat, maxLon, maxLat);
        rc.sceneBounds.expand(proj::lonToX(minLon), -proj::latToY(minLat));
        rc.sceneBounds.expand(proj::lonToX(maxLon), -proj::latToY(maxLat));

        parsed.insert(rc.id, std::move(chart));
        out.push_back(std::move(rc));
    }

    progress(paths.size(), paths.size());

    if (out.empty()) {
        errMsg = skipped ? QStringLiteral("no usable KAP charts under %1 (%2 skipped; first: %3)")
                               .arg(root).arg(skipped).arg(firstSkip)
                         : QStringLiteral("no usable KAP charts under %1").arg(root);
        return false;
    }
    if (skipped)
        errMsg = QStringLiteral("KAP: skipped %1 unsupported chart(s); first: %2")
                     .arg(skipped).arg(firstSkip);

    {
        QWriteLocker guard(&lock_);
        charts_ = std::move(parsed);
    }
    qInfo().noquote() << QStringLiteral("KAP: catalogued %1 chart(s) under %2")
                             .arg(out.size()).arg(root);
    return true;
}

bool KapChartSource::tile(const QString& chartId, int z, int x, int y,
                          QImage& out, QString& errMsg) {
    std::shared_ptr<const KapChart> chart;
    {
        QReadLocker guard(&lock_);
        chart = charts_.value(chartId);
    }
    if (!chart) {
        errMsg = QStringLiteral("unknown chart id %1").arg(chartId);
        return false;
    }
    // Rendering touches only the (immutable) chart and its own file handle, so
    // it runs unlocked and concurrently with other tiles.
    return chart->renderTile(z, x, y, kTilePx, out, errMsg);
}
