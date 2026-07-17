// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart plugin for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#include "kap_plugin.hpp"
#include "plugin_factory.hpp"

#include <QObject>

// QPluginLoader entry point for the KAP plugin DLL. Mirrors the other plugins'
// factories: the only QObject the host instantiates; it validates ABI/metadata
// before handing back the real IPlugin.
class KapPluginFactory : public QObject, public IPluginFactory {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID CHARTPLOTTER_PLUGIN_IID FILE "kap_plugin.json")
    Q_INTERFACES(IPluginFactory)
public:
    int abiVersion() const override { return kPluginAbiVersion; }
    std::unique_ptr<IPlugin> create() override {
        return std::make_unique<KapPlugin>();
    }
};

#include "kap_plugin_factory.moc"
