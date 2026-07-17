// SPDX-License-Identifier: GPL-2.0-or-later
//
// BSB/KAP raster chart plugin for HMV Chartplotter.
// Copyright (C) 2026 Warren Holybee. See README.md / COPYING.
#include "kap_plugin.hpp"

#include "kap_chart_source.hpp"

#include <QtGlobal>

KapPlugin::KapPlugin() = default;
KapPlugin::~KapPlugin() = default;   // out-of-line: KapChartSource complete here

void KapPlugin::initialize(ICoreApi* core) {
    core_ = core;
    source_ = std::make_unique<KapChartSource>();
    core_->registerRasterChartSource(source_.get());
    qInfo("KAP plugin: registered raster chart source (BSB/KAP 2.0).");
}

void KapPlugin::shutdown() {
    // Unregister before the source object is destroyed: the host drains any
    // in-flight tile renders that hold a pointer to it (see
    // ChartView::onRasterChartSourceUnregistered).
    if (core_ && source_) core_->unregisterRasterChartSource(source_.get());
    source_.reset();
    core_ = nullptr;
}
