#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A projected point, in Mercator metres.
struct Pt {
    double x;
    double y;
};

// Axis-aligned bounding box in projected coordinates (north-up: +y is north).
struct BBox {
    double minx =  1e30, miny =  1e30;
    double maxx = -1e30, maxy = -1e30;

    void expand(double x, double y) {
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    void expand(const BBox& b) {
        if (!b.valid()) return;
        expand(b.minx, b.miny);
        expand(b.maxx, b.maxy);
    }
    bool valid() const { return maxx >= minx && maxy >= miny; }
    bool intersects(const BBox& o) const {
        return !(o.minx > maxx || o.maxx < minx || o.miny > maxy || o.maxy < miny);
    }
    // True if every point of o lies within this box (used to decide whether a
    // feature/cell is already fully inside a clip region, so clipping can be
    // skipped, and to detect when a cached clip no longer covers the view).
    bool contains(const BBox& o) const {
        return valid() && o.valid() &&
               o.minx >= minx && o.maxx <= maxx &&
               o.miny >= miny && o.maxy <= maxy;
    }
};

enum class FeatureKind {
    DepthArea, LandArea, OtherArea,
    DepthContour, Coastline, OtherLine,
    Sounding, Point
};

struct Feature {
    FeatureKind kind = FeatureKind::Point;
    int zorder = 0;
    std::vector<std::vector<Pt>> rings;
    double depth = 0.0;
    bool hasDepth = false;
    // S-57 SCAMIN: the smallest display scale (largest denominator) at which the
    // object should be drawn. 0 = attribute absent (object has no scale floor and
    // is always eligible). Used at paint time to declutter point objects as the
    // view zooms out. See ChartView::scaminPasses.
    int scaleMin = 0;
    BBox bbox;
    // S-57 object-class name (e.g. "BOYLAT", "ACHARE"). Populated for Point and
    // OtherArea features (the symbol-bearing kinds); empty otherwise. Used at
    // cell-build time to resolve a symbol via the S-52 lookup engine.
    std::string objClass;
    // Symbology-relevant S-57 attributes: (6-char acronym, value string), e.g.
    // {"BOYSHP","4"},{"COLOUR","4"}. Only the attributes the lookup tables
    // actually test are read (see chart::setSymbologyAttrs). Drives best-match
    // symbol selection. Empty for non-symbol features.
    std::vector<std::pair<std::string, std::string>> attrs;
    // S-57 OBJNAM (object name), UTF-8. Populated for symbol-bearing features
    // when present; drawn as a text label next to the object. Empty when the
    // object has no name.
    std::string name;
};

namespace chart {

// Call once at startup (registers GDAL drivers + sets S-57 options). Safe before
// spawning worker threads; the config it sets is process-global.
//
// Pass `gdalDataDir` to override the GDAL_DATA search path — required on
// machines where GDAL is not installed system-wide. Provide the path to a
// folder containing s57objectclasses.csv (bundled as gdal-data/ next to the
// exe by the CMake build). Without it GDAL can still read geometry but cannot
// resolve S-57 object-class names, so charts render without colour or fill.
void init(const std::string& gdalDataDir = {});

// Tell the loader which S-57 attribute acronyms the symbology engine tests, so
// loadCellFeatures reads exactly those into Feature::attrs (and no others).
// Call once after the symbol atlas is loaded and before any cell load. The set
// is process-global and read-only during loads, so it needs no locking.
// Passing an empty list disables attribute reading (symbols fall back to the
// class default / dot).
void setSymbologyAttrs(const std::vector<std::string>& acronyms);

// Read all geometry of one ENC cell into `out` (projected), with the cell's
// bbox. Thread-safe: opens and closes its own GDAL handle. Heavy — call from a
// worker thread.
bool loadCellFeatures(const std::string& path,
                      std::vector<Feature>& out, BBox& bbox, std::string& err);

// Cheaply determine a cell's geographic extent (longitude/latitude degrees),
// preferring the small M_COVR coverage layer. Thread-safe. Used to build the
// catalog without reading full geometry.
bool computeCellExtentLonLat(const std::string& path,
                             double& minLon, double& minLat,
                             double& maxLon, double& maxLat, std::string& err);

// Read the cell's actual data-coverage footprint from the M_COVR layer:
// the exterior rings of every CATCOV=1 (coverage-available) polygon, projected
// to Mercator metres, with their combined bounding box. This is the true cell
// outline — usually a small polygon, often a diagonal sliver, not the bbox.
// Thread-safe. Returns false when no usable M_COVR geometry is present (the
// caller then falls back to the plain extent box as the coverage). Holes
// (inner rings / CATCOV=2) are intentionally ignored: over-covering is the safe
// direction for quilting (it can never leave a duplicate symbol showing).
bool computeCellCoverage(const std::string& path,
                         std::vector<std::vector<Pt>>& rings, BBox& bbox,
                         std::string& err);

// Load a GSHHG basemap tier into projected features: GSHHS L1 land polygons
// (LandArea) and L2 lakes (DepthArea, drawn as water). `gshhgRoot` is the folder
// containing GSHHS_shp/<tier>/; `tier` is one of c/l/i/h/f. Heavy — call from a
// worker thread. Returns false if the L1 shapefile can't be opened.
bool loadBasemap(const std::string& gshhgRoot, const std::string& tier,
                 std::vector<Feature>& out, std::string& err);

} // namespace chart
