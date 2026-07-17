#pragma once
#include <QObject>
#include <QtPlugin>
#include <memory>

class IPlugin;

// Plugin ABI version. Bumped whenever a virtual method on ICoreApi/IPlugin or
// the layout of a type crossing the boundary changes. Plugins compiled against
// a different version are rejected by the loader before any vtable touch.
//
// v3: added ICoreApi::registerChartSource/unregisterChartSource (and the
//     IChartSource / ChartSourceCell value types in chart_source.hpp) so a
//     plugin can supply vector charts (e.g. CM93) through the existing pipeline.
// v4: added a progress callback parameter to IChartSource::catalog so a heavy
//     scan (e.g. CM93's first-run decode of every cell) can report progress.
// v5: added ICoreApi::registerRasterChartSource/unregisterRasterChartSource (and
//     the IRasterChartSource / RasterSourceChart value types in
//     raster_chart_source.hpp) so a plugin can supply raster charts (e.g. BSB/
//     KAP) through the existing MBTiles raster layer. Also formalises
//     ChartViewport::upDegrees, which was added to the struct during v4 without
//     a bump — v4 plugins built against the pre-upDegrees header therefore have
//     a short view of the struct, and this bump forces them to re-vendor.
inline constexpr int kPluginAbiVersion = 5;

// Q_DECLARE_INTERFACE IID. Must match the IID in each plugin's
// Q_PLUGIN_METADATA(); change in lock-step with kPluginAbiVersion.
#define CHARTPLOTTER_PLUGIN_IID "com.chartplotter.IPluginFactory/5.0"

// The single symbol a dynamic plugin DLL exposes to the host. The plugin's
// shared library contains exactly one QObject implementing this interface; the
// host instantiates that QObject through QPluginLoader, calls abiVersion() as
// a defence-in-depth check (the metadata is checked first, without mapping the
// binary), and then asks the factory to construct the actual IPlugin.
//
// Keeping the factory separate from the IPlugin lets plugins keep their main
// classes free of QObject inheritance — most of our existing plugins (NMEA
// 0183/2000, MBTiles etc.) inherit from IPlugin and ISettingsPageProvider and
// would otherwise need a diamond.
class IPluginFactory {
public:
    virtual ~IPluginFactory() = default;
    virtual int abiVersion() const = 0;
    virtual std::unique_ptr<IPlugin> create() = 0;
};

Q_DECLARE_INTERFACE(IPluginFactory, CHARTPLOTTER_PLUGIN_IID)
