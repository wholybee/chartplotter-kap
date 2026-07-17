#pragma once
#include <QImage>
#include <QString>
#include <algorithm>
#include <functional>
#include <vector>
#include "chart_loader.hpp"   // BBox

// ============================================================================
// Pluggable raster-chart backend.
// ============================================================================
//
// The built-in MBTiles reader (mbtiles_reader.cpp / mbtiles_service.cpp) is the
// default way raster imagery enters the pipeline. A plugin can supply an
// additional backend — e.g. a BSB/KAP reader — by implementing IRasterChartSource
// and handing it to the host via ICoreApi::registerRasterChartSource(). The rest
// of the raster layer (tile selection, the pixmap LRU, coarser-ancestor
// fallback, the painter blit, and the GPU textured-quad path) is unchanged: a
// source plugs in by serving the same XYZ tiles the MBTiles path already does.
//
// This is the raster twin of IChartSource (chart_source.hpp), and deliberately
// mirrors it — with two differences worth knowing:
//
//  1. Raster sources are ADDITIVE, not exclusive. The vector pipeline picks one
//     backend per scan (a quilt can only come from one), so ChartSourceRegistry
//     has pick(). Raster charts just stack, so every registered source is offered
//     every selected folder and all of their charts are drawn together — exactly
//     how the built-in MBTiles layer already scans every selected folder
//     regardless of which vector backend won.
//
//  2. The unit of work is an XYZ tile, not a cell. Sources whose native storage
//     is not tiled (a KAP is one large georeferenced image) resample into the
//     tile grid on demand. Serving tiles rather than "a big image plus a
//     transform" is what lets the host's existing pyramid/cache/GPU machinery
//     apply unchanged.
//
// Threading: catalog() and tile() run on a host worker thread and MUST be
// thread-safe; tile() may be called concurrently for different tiles. The host
// owns all threading, caching, and staleness (generation tokens) — a source is a
// passive, synchronous provider and needs no Qt threading of its own.

// One raster chart advertised by a source during cataloging.
struct RasterSourceChart {
    // Opaque chart identity, unique within the source. Round-tripped verbatim to
    // tile(); the host never parses it. For a KAP source this is typically the
    // file path.
    QString id;
    // Human label (status text / UI).
    QString name;
    // The zoom range this chart can serve. The host clamps its natural zoom (the
    // one whose tile edge ≈ 256 device px) into this range, so:
    //   maxZoom should be the chart's NATIVE resolution — past it the host
    //     upscales one tile rather than requesting more of them.
    //   minZoom bounds how far out the source can downsample. Set it low enough
    //     that a zoomed-out view asks for a few coarse tiles instead of hundreds
    //     of native ones.
    int minZoom = 0;
    int maxZoom = 19;
    // Chart coverage projected into the app's scene frame — the same frame
    // BuiltPath geometry and MbtilesMeta::sceneBounds live in: x = lonToX(lon),
    // y = -latToY(lat) (Web Mercator metres, north-up). Tiles outside it are
    // never requested. Leave invalid() only if the coverage is genuinely unknown
    // (the host then cannot cull, and asks for tiles across the whole view).
    BBox sceneBounds;
};

class IRasterChartSource {
public:
    virtual ~IRasterChartSource() = default;

    // Stable identifier (e.g. "kap") and human label for status text / UI.
    virtual QString sourceId() const = 0;
    virtual QString displayName() const = 0;

    // Cheap test: does `root` hold charts this source reads? Check only a
    // directory signature (a marker file / a matching extension present), don't
    // parse anything heavy — this is called for every registered source on every
    // chart-set switch. Unlike the vector side this is not winner-takes-all:
    // every source that claims a folder gets to catalog it.
    virtual bool canHandle(const QString& root) const = 0;

    // Enumerate every chart under `root` with its footprint and zoom range. Runs
    // on a worker thread; should stay cheap (headers only — never decode
    // imagery here). Returns false (and sets errMsg) on failure.
    //
    // `progress(done, total)` may be called periodically during a long scan to
    // drive the host's progress UI; it is safe to ignore. It is invoked on the
    // scan worker thread.
    virtual bool catalog(const QString& root,
                         std::vector<RasterSourceChart>& out, QString& errMsg,
                         const std::function<void(int done, int total)>& progress) = 0;

    // Render one XYZ tile of `chartId` — the slippy-map convention, y = 0 at the
    // north, 2^z tiles per axis, in Web Mercator (EPSG:3857). `out` should be a
    // 256x256 image; the host copes with any size, treating it as the tile's full
    // footprint. Return an image with alpha where the chart doesn't cover the
    // tile — the host composites tiles over the basemap.
    //
    // Return true with a null `out` for "this tile is legitimately empty" (the
    // host caches that as absent and never asks again this generation). Return
    // false + errMsg only for a real failure.
    //
    // Runs on a worker thread; must be thread-safe across concurrent calls.
    virtual bool tile(const QString& chartId, int z, int x, int y,
                      QImage& out, QString& errMsg) = 0;
};

// ---------------------------------------------------------------------------
// Host-side registry of raster chart sources (not used by plugins). Owned by
// MainWindow; CoreApi forwards register/unregister here, and ChartView snapshots
// sources() on the GUI thread when a scan starts, handing the snapshot to its
// raster worker (so the worker never touches the registry).
// ---------------------------------------------------------------------------
class RasterChartSourceRegistry {
public:
    void add(IRasterChartSource* s) {
        if (s && std::find(sources_.begin(), sources_.end(), s) == sources_.end())
            sources_.push_back(s);
    }
    void remove(IRasterChartSource* s) {
        sources_.erase(std::remove(sources_.begin(), sources_.end(), s),
                       sources_.end());
    }
    const std::vector<IRasterChartSource*>& sources() const { return sources_; }

private:
    std::vector<IRasterChartSource*> sources_;
};
