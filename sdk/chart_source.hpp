#pragma once
#include <QString>
#include <algorithm>
#include <functional>
#include <vector>
#include "chart_loader.hpp"   // Feature, BBox, Pt

// ============================================================================
// Pluggable vector-chart backend.
// ============================================================================
//
// The built-in ENC/S-57 reader (chart_loader.cpp, GDAL-based) is the default
// way cells enter the pipeline. A plugin can register an alternative backend —
// e.g. a CM93 reader — by implementing IChartSource and handing it to the host
// via ICoreApi::registerChartSource(). The rest of the app (catalog, quilting,
// FeatureCache LRU, clip/build, S-52 symbology, paint) is unchanged: a chart
// source plugs in by producing the same value types the ENC path already does.
//
// The two methods mirror the built-in path one-to-one:
//
//   catalog()  ~ ChartCatalog's *.000 enumeration + chart::computeCellCoverage
//                (cheap per-cell footprints; no full geometry parse)
//   loadCell() ~ chart::loadCellFeatures (the full per-cell parse)
//
// Both run on worker threads and MUST be thread-safe: catalog() is called once
// per scan, loadCell() concurrently for different cells. Output geometry is
// projected to Mercator metres (see projection.hpp). Crucially, features carry
// S-57 object-class acronyms and attributes (Feature::objClass / ::attrs), so a
// non-S-57 source (CM93) must translate its native object/attribute dictionary
// onto S-57 acronyms; the host's S-52 symbology engine then resolves them with
// no special-casing.

// One cell advertised by a source during cataloging. Deliberately decoupled
// from the QObject-bearing CellRecord (chart_catalog.hpp) so a plugin need not
// pull the catalog header; the catalog converts ChartSourceCell -> CellRecord.
struct ChartSourceCell {
    // Opaque cell identity, unique within the source. Round-tripped verbatim to
    // loadCell() and used as the host's per-cell cache/loaded key (it stands in
    // for the file path the ENC path uses). For CM93 this is typically the
    // containing file path plus a sub-cell index, encoded however the source
    // likes — the host never parses it.
    QString id;
    // Normalized usage band: 1 = overview .. 6 = berthing, 0 = unknown. Sources
    // with a different scale model (CM93 has 8 scales, Z and A..G) map their
    // native scale onto this range so the host's band logic (bandForVisibleWidth,
    // gap-fill quilting) works without modification.
    int     band = 0;
    // Cell footprint, projected Mercator (north-up: +y north).
    BBox    bbox;
    // Exterior coverage rings (projected). Empty => the host treats the bbox as
    // the coverage, exactly as it does for ENC cells lacking an M_COVR layer.
    // Drives quilting (only the finest band draws in any region).
    std::vector<std::vector<Pt>> coverage;
};

class IChartSource {
public:
    virtual ~IChartSource() = default;

    // Stable identifier (e.g. "cm93") and human label for status text / UI.
    virtual QString sourceId() const = 0;
    virtual QString displayName() const = 0;

    // Cheap test: does `root` hold charts this source reads? Implementations
    // should check only a directory signature (presence of marker files /
    // subdirs), not parse anything heavy — this is called for each registered
    // source on every chart-set switch. The host uses the first source that
    // returns true; if none do, it falls back to the built-in ENC reader.
    virtual bool canHandle(const QString& root) const = 0;

    // Enumerate every cell under `root` with its footprint. Cheap relative to a
    // full parse, but may still be slow on a cold first run; runs on a worker
    // thread. Returns false (and sets errMsg) on failure. The source owns any
    // on-disk catalog caching it wants to add.
    //
    // `progress(done, total)` may be called periodically during a long scan (to
    // drive the host's progress UI); it is safe to ignore. It is invoked on the
    // scan worker thread.
    virtual bool catalog(const QString& root,
                         std::vector<ChartSourceCell>& out, QString& errMsg,
                         const std::function<void(int done, int total)>& progress) = 0;

    // Parse one cell (identified by ChartSourceCell::id) into projected Features
    // plus the cell's bbox. Runs on a worker thread and must be thread-safe
    // across concurrent calls for different cells. Returns false (and sets
    // errMsg) on failure.
    virtual bool loadCell(const QString& cellId,
                          std::vector<Feature>& out, BBox& bbox,
                          QString& errMsg) = 0;
};

// ---------------------------------------------------------------------------
// Host-side registry of chart sources (not used by plugins). Owned by
// MainWindow; CoreApi forwards register/unregister here, and MainWindow asks
// pick() which source — if any — claims a chart folder when a scan starts.
// ---------------------------------------------------------------------------
class ChartSourceRegistry {
public:
    void add(IChartSource* s) {
        if (s && std::find(sources_.begin(), sources_.end(), s) == sources_.end())
            sources_.push_back(s);
    }
    void remove(IChartSource* s) {
        sources_.erase(std::remove(sources_.begin(), sources_.end(), s),
                       sources_.end());
    }
    // First registered source that claims `root`, or nullptr for none (the
    // caller then uses the built-in ENC reader).
    IChartSource* pick(const QString& root) const {
        for (IChartSource* s : sources_)
            if (s && s->canHandle(root)) return s;
        return nullptr;
    }
    const std::vector<IChartSource*>& sources() const { return sources_; }

private:
    std::vector<IChartSource*> sources_;
};
