// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart plugin for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#pragma once

#include <QString>
#include <memory>

#include "plugin_api.hpp"

class KapChartSource;

// Registers a BSB/KAP backend with the host's raster chart layer. The plugin
// itself is a thin shell: all the work is in KapChartSource.
class KapPlugin : public IPlugin {
public:
    KapPlugin();
    ~KapPlugin() override;

    QString name() const override    { return QStringLiteral("BSB/KAP Charts"); }
    QString version() const override { return QStringLiteral("0.1.0"); }

    void initialize(ICoreApi* core) override;
    void shutdown() override;

private:
    ICoreApi*                       core_ = nullptr;
    std::unique_ptr<KapChartSource> source_;
};
