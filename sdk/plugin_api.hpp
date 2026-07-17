#pragma once
#include <QString>
#include <QPointF>
#include <QSize>
#include <QTransform>
#include <QVariant>
#include <functional>
#include <cmath>
#include "projection.hpp"

class QPainter;
class QWidget;
class INavDataPublisher;
class NavDataStore;
class IAisPublisher;
class AisTargetStore;
class RouteStore;
class IChartSource;
class IRasterChartSource;

// Plugin API surface (Milestone 3 in ProjectSpec.md).
//
// Built-in plugins use exactly the interfaces a dynamic (DLL/SO) plugin will use
// later, so the contract can be refined before ABI/loader concerns appear.
// Plugins never touch core internals directly: they publish through stable APIs,
// subscribe to core signals, contribute menu items, and draw via overlays — all
// through ICoreApi, which the core implements.

// ---- coordinate / viewport bridge -----------------------------------------

// A snapshot of the chart camera handed to overlays so they can place drawing in
// geographic terms without knowing how the canvas is implemented. Value type:
// valid only for the duration of one paint call.
struct ChartViewport {
    QTransform sceneToScreen;   // projected scene metres (Y down) -> device px
    double     ppm = 0.0;       // pixels per scene metre (zoom)
    QSize      size;            // viewport size in device px
    double     worldWidthM = 0.0;
    double     centerSceneX = 0.0;
    // Course-up rotation: the compass bearing (deg true) currently pointing to
    // the top of the view (0 = north-up). sceneToScreen already bakes this in for
    // positions; overlays that orient a glyph by a true bearing must subtract it
    // (a bearing points (bearing - upDegrees) clockwise from screen-up).
    double     upDegrees = 0.0;

    QSize  viewportSize()   const { return size; }
    double pixelsPerMetre() const { return ppm; }

    // Geographic <-> screen. geoToScreen picks the nearest world copy across the
    // 180 degree seam so points near the date line land on-screen.
    QPointF geoToScreen(double latDeg, double lonDeg) const {
        double sx = proj::lonToX(lonDeg);
        const double sy = -proj::latToY(latDeg);
        if (worldWidthM > 0.0)
            sx += std::round((centerSceneX - sx) / worldWidthM) * worldWidthM;
        return sceneToScreen.map(QPointF(sx, sy));
    }
    void screenToGeo(const QPointF& px, double& latDeg, double& lonDeg) const {
        const QPointF s = sceneToScreen.inverted().map(px);
        lonDeg = proj::xToLon(s.x());
        latDeg = proj::yToLat(-s.y());
    }
};

// Handle returned when a plugin registers as a navigation data source. The
// plugin uses it to drive the status dot next to its auto-created menu item.
// Owned by the core; valid until shutdown.
class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual void setActive(bool on) = 0;   // green dot on/off (plugin-controlled)
};

// Persistent per-plugin settings, namespaced by plugin id, backed by the same
// store the core uses (survives restarts). Owned by the core.
class IPluginSettings {
public:
    virtual ~IPluginSettings() = default;
    virtual void     setValue(const QString& key, const QVariant& value) = 0;
    virtual QVariant value(const QString& key, const QVariant& def = QVariant()) const = 0;
};

// A plugin's settings page. The plugin supplies only the content widget; the
// core hosts it (window chrome, title, parenting, single-instance) so plugins
// don't manage their own dialogs. Register via ICoreApi::addSettingsPage().
class ISettingsPageProvider {
public:
    virtual ~ISettingsPageProvider() = default;
    virtual QString  settingsPageTitle() const = 0;
    // Build the page content, parented to `parent`. The core takes ownership.
    virtual QWidget* createSettingsPage(QWidget* parent) = 0;
};

// A chart overlay: the controlled way for a plugin to draw on the canvas without
// owning scene items, z-order, or threading. The core calls paint() each frame
// after its own drawing, in device coordinates.
class IChartOverlay {
public:
    virtual ~IChartOverlay() = default;
    virtual void paint(QPainter& painter, const ChartViewport& viewport) = 0;
    // Optional pick support; default declines. (screen pixel hit.)
    virtual bool hitTest(const QPointF& /*screenPt*/) { return false; }
};

// ---- core services handed to plugins ---------------------------------------

// Stable services a plugin receives in initialize(). The core owns the nav
// store, chart canvas, menu, settings, and lifetimes; plugins act through here.
class ICoreApi {
public:
    virtual ~ICoreApi() = default;

    // Navigation data --------------------------------------------------------
    // Publish updates (per-value source/timestamp arbitration applies).
    virtual INavDataPublisher* navPublisher() = 0;
    // Read current state and connect to its ownshipChanged() signal to subscribe.
    virtual const NavDataStore* navData() const = 0;

    // AIS targets ------------------------------------------------------------
    // Publish AIS targets (e.g. from an AIS decoder plugin).
    virtual IAisPublisher* aisPublisher() = 0;
    // Read targets; connect to targetUpdated/targetExpired to subscribe.
    virtual const AisTargetStore* aisData() const = 0;

    // Routes & waypoints -----------------------------------------------------
    // Read, create, edit, and delete routes and standalone waypoints, backed by
    // the same persistent store (routes.db) the core uses. Unlike nav/AIS there
    // is no source arbitration, so reading and writing share one handle:
    //   read   -> routes()->routes() / waypoints() / route(id) (cached snapshots)
    //   write  -> addRoute/updateRoute/removeRoute, addWaypoint/... (DB + cache)
    //   notify -> connect routesChanged()/waypointsChanged() to subscribe
    // The core owns the store; the plugin holds a non-owning pointer. May be
    // null if the store failed to open (check before use).
    virtual RouteStore* routes() = 0;

    // Menu contributions -----------------------------------------------------
    // Append items to the main menu's Plugins section.
    virtual void addMenuAction(const QString& title,
                               std::function<void()> onTriggered) = 0;
    virtual void addMenuToggle(const QString& title, bool checked,
                               std::function<void(bool)> onToggled) = 0;

    // Persistent per-plugin settings, namespaced by `pluginId`. Core-owned.
    virtual IPluginSettings* pluginSettings(const QString& pluginId) = 0;

    // Settings pages -------------------------------------------------------
    // Contribute a settings page: the core adds an item under Settings >
    // Plugin Settings that hosts the provider's page. showSettingsPage opens
    // that page on demand (e.g. from a data-source item's click).
    virtual void addSettingsPage(ISettingsPageProvider* provider) = 0;
    virtual void showSettingsPage(ISettingsPageProvider* provider) = 0;

    // Data sources -----------------------------------------------------------
    // Register the plugin as a navigation data source. `sourceId` is the stable
    // id the plugin stamps on its published values (and how it appears in Data
    // Priority); `name` is the display name. The core adds an item under
    // Settings > Data Connections; clicking it invokes `onOpenSettings` (the
    // plugin shows its own settings dialog). The returned handle lets the plugin
    // drive its status dot. Core owns the handle.
    virtual IDataSource* registerDataSource(const QString& sourceId, const QString& name,
                                            std::function<void()> onOpenSettings) = 0;

    // Chart overlays ---------------------------------------------------------
    virtual void addChartOverlay(IChartOverlay* overlay) = 0;
    virtual void removeChartOverlay(IChartOverlay* overlay) = 0;
    virtual void requestChartRepaint() = 0;

    // Chart sources ----------------------------------------------------------
    // Register a pluggable vector-chart backend (e.g. CM93). See IChartSource
    // in chart_source.hpp. The host offers each registered source the active
    // chart folder via IChartSource::canHandle and uses the first that claims
    // it; the built-in ENC/S-57 reader is the fallback. The plugin owns the
    // IChartSource object and MUST unregister it in shutdown() before the
    // object is destroyed.
    virtual void registerChartSource(IChartSource* source) = 0;
    virtual void unregisterChartSource(IChartSource* source) = 0;

    // Raster chart sources ---------------------------------------------------
    // Register a pluggable raster-chart backend (e.g. BSB/KAP). See
    // IRasterChartSource in raster_chart_source.hpp. Unlike vector sources these
    // are additive: every registered source is offered every selected chart
    // folder, and the charts of all of them draw together in the raster layer
    // alongside the built-in MBTiles charts. The plugin owns the
    // IRasterChartSource object and MUST unregister it in shutdown() before the
    // object is destroyed.
    virtual void registerRasterChartSource(IRasterChartSource* source) = 0;
    virtual void unregisterRasterChartSource(IRasterChartSource* source) = 0;

    // A parent for plugin-created dialogs/windows.
    virtual QWidget* dialogParent() = 0;
};

// ---- the plugin itself ------------------------------------------------------

class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual QString name() const = 0;
    virtual QString version() const = 0;
    // Register contributions (menu items, overlays) and grab core handles.
    virtual void initialize(ICoreApi* core) = 0;
    // Release anything registered with the core.
    virtual void shutdown() = 0;
};
