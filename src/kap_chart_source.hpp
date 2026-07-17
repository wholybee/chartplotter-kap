// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart source for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#pragma once

#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <memory>
#include <vector>

#include "kap_reader.hpp"
#include "raster_chart_source.hpp"

// Serves BSB/KAP charts to the host's raster layer.
//
// catalog() parses every *.kap under the chart folder (headers only — cheap) and
// keeps the parsed charts; tile() then renders XYZ tiles from them on demand.
// Chart ids are file paths, so they survive the host re-cataloguing.
//
// Threading: the host calls catalog() once per scan and tile() concurrently on
// worker threads. Parsed charts are immutable, so tile rendering needs no lock
// at all; the only shared mutable state is the chart map itself, which a
// read/write lock guards (written once per catalog, read per tile).
class KapChartSource : public IRasterChartSource {
public:
    KapChartSource();
    ~KapChartSource() override;

    QString sourceId() const override    { return QStringLiteral("kap"); }
    QString displayName() const override { return QStringLiteral("BSB/KAP Charts"); }

    bool canHandle(const QString& root) const override;

    bool catalog(const QString& root, std::vector<RasterSourceChart>& out,
                 QString& errMsg,
                 const std::function<void(int done, int total)>& progress) override;

    bool tile(const QString& chartId, int z, int x, int y,
              QImage& out, QString& errMsg) override;

private:
    mutable QReadWriteLock lock_;
    // chart id (file path) -> parsed chart. shared_ptr so tile() can take a
    // reference out from under the lock and render without holding it.
    QHash<QString, std::shared_ptr<const KapChart>> charts_;
};
