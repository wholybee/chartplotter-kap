# BSB/KAP raster chart plugin for HMV Chartplotter — standalone build.
#
# Builds a single dynamically-loaded Qt MODULE that the host application loads at
# runtime via QPluginLoader. It depends ONLY on Qt6 and the host's plugin-SDK
# headers (vendored in sdk/) — no GDAL, no libbsb, and no link against the host.
# See README.md and sdk/README.md.
#
# This plugin is GPL-2.0-or-later; the host application remains under its own
# LGPL-2.1 licence. See COPYING.
cmake_minimum_required(VERSION 3.21)
project(chartplotter_kap_plugin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

# The host plugin-SDK headers (the ABI contract): raster_chart_source.hpp,
# plugin_api.hpp, plugin_factory.hpp, chart_loader.hpp, projection.hpp. Vendored
# in sdk/; keep plugin_factory.hpp's kPluginAbiVersion + IID identical to the
# host you target. See sdk/README.md.
set(CHARTPLOTTER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH
    "Directory containing the chartplotter plugin-SDK headers")

# ---- the plugin -------------------------------------------------------------
add_library(kap_plugin MODULE
    src/kap_plugin.hpp
    src/kap_plugin.cpp
    src/kap_plugin_factory.cpp
    src/kap_chart_source.hpp
    src/kap_chart_source.cpp
    src/kap_reader.hpp
    src/kap_reader.cpp
    src/kap_plugin.json
)

set_target_properties(kap_plugin PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME "chartplotter_kap_plugin"    # stable name in the host plugins/ dir
    PREFIX ""                                # no "lib" prefix on non-Windows
)

target_include_directories(kap_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CHARTPLOTTER_SDK_DIR}"
)

# No host library is linked: the plugin uses the SDK headers only (pure-virtual
# interfaces + POD/inline types). Its import table is just Qt6Core + Qt6Gui
# (QImage). Widgets are not needed — this plugin contributes no UI.
target_link_libraries(kap_plugin PRIVATE
    Qt6::Core
    Qt6::Gui
)

# ---- optional: install into an existing host build/install -------------------
# Set to the directory containing hmvchartplotter.exe. The plugin module is
# copied into its plugins/ subfolder.
set(CHARTPLOTTER_APP_DIR "" CACHE PATH
    "Installed host app directory (the folder with hmvchartplotter.exe)")
if(CHARTPLOTTER_APP_DIR)
    add_custom_command(TARGET kap_plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CHARTPLOTTER_APP_DIR}/plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:kap_plugin>" "${CHARTPLOTTER_APP_DIR}/plugins/"
        COMMENT "Installing KAP plugin into ${CHARTPLOTTER_APP_DIR}"
        VERBATIM
    )
endif()

# ---- optional: standalone reader smoke test ---------------------------------
# A console tool that parses a folder of real KAP charts, checks the
# georeference, and renders sample tiles to PNG (no GUI, no host).
# Run: test_kap <KAP folder> [outdir]   (needs Qt6Core/Qt6Gui DLLs on PATH).
option(BUILD_KAP_TESTS "Build the KAP reader smoke test" ON)
if(BUILD_KAP_TESTS)
    add_executable(test_kap
        test/test_kap.cpp
        src/kap_reader.cpp
    )
    set_target_properties(test_kap PROPERTIES AUTOMOC OFF WIN32_EXECUTABLE FALSE)
    target_include_directories(test_kap PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CHARTPLOTTER_SDK_DIR}")
    target_link_libraries(test_kap PRIVATE Qt6::Core Qt6::Gui)
endif()
