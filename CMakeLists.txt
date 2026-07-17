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
project(chartplotter_kap_plugin VERSION 1.0.0 LANGUAGES CXX)

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

# ---- install rules (used by the NSIS installer, and by `cmake --install`) ----
#
# The installer overlays an existing host installation, so its layout is relative
# to the host's install root ($PROGRAMFILES64\HMV Chartplotter on Windows):
#
#   plugins/chartplotter_kap_plugin.dll   the loadable module (QPluginLoader)
#
# A MODULE library is a LIBRARY artifact on every platform (including the .dll on
# Windows), so LIBRARY is the destination that matters; RUNTIME is named too for
# belt-and-suspenders across CMake versions.
install(TARGETS kap_plugin
        LIBRARY DESTINATION "plugins"
        RUNTIME DESTINATION "plugins")

# Bundling Qt DLLs is OFF by default, on purpose. The host is a Qt Widgets
# application, so it already ships Qt6Core.dll and Qt6Gui.dll (and the whole
# windeployqt tree: platforms/, imageformats/, tls/, ...) next to its exe — this
# plugin links nothing the host doesn't already provide (verified: every DLL
# windeployqt lists for the plugin is present in the host deployment). Two
# reasons not to ship them anyway:
#   1. Redundant — the host's copies are already on the plugin's DLL search path
#      (the plugin loads into the host process, which resolves Qt from its exe
#      dir).
#   2. Dangerous — if the plugin dropped its own Qt6Core.dll next to the host's
#      and the user later uninstalled the PLUGIN, the plugin's uninstaller would
#      delete a DLL the HOST needs, breaking the host. A plugin uninstaller must
#      never remove host-owned files.
# Turn this ON only for a host-less / self-contained deployment, where there is
# no host Qt runtime to collide with. (The genuine "the plugin needs a Qt module
# the host lacks" case — e.g. the CM93 plugin's Qt6Concurrent — is different:
# there you bundle *only that one extra module*, which the host never had and so
# is safe to remove on uninstall. List such modules in
# CHARTPLOTTER_KAP_EXTRA_RUNTIME_DLLS below; for this plugin the set is empty.)
option(CHARTPLOTTER_KAP_INSTALLER_BUNDLE_QT
    "Bundle Qt6Core/Qt6Gui next to the host exe (host-less installs only; unsafe when a host is present)" OFF)
if(WIN32 AND CHARTPLOTTER_KAP_INSTALLER_BUNDLE_QT)
    install(FILES "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Gui>"
            DESTINATION "." CONFIGURATIONS Release)
endif()

# Extra runtime DLLs to bundle into the install root — for the rare case where
# the plugin links a Qt (or other) module the host does not deploy. Empty for
# this plugin. Pass absolute paths, semicolon-separated:
#   -DCHARTPLOTTER_KAP_EXTRA_RUNTIME_DLLS="C:/Qt/.../Qt6Foo.dll"
set(CHARTPLOTTER_KAP_EXTRA_RUNTIME_DLLS "" CACHE STRING
    "Absolute paths of extra runtime DLLs to install next to the host exe")
if(WIN32 AND CHARTPLOTTER_KAP_EXTRA_RUNTIME_DLLS)
    install(FILES ${CHARTPLOTTER_KAP_EXTRA_RUNTIME_DLLS}
            DESTINATION "." CONFIGURATIONS Release)
endif()

# ---- Packaging: Windows installer (CPack + NSIS) ----------------------------
#
# Wraps the plugin (and any bundled runtime DLLs) into a single NSIS installer
# .exe that overlays an existing HMV Chartplotter installation: it drops the
# module into <host>\plugins\ so QPluginLoader finds it on next launch.
#
# Usage — build Release first, then run cpack from the build dir:
#
#   cmake --build build/vs2022 --config Release
#   cpack --config build/vs2022/CPackConfig.cmake -C Release -G NSIS
#
# (or just build Release: the `installer` target below folds cpack into the build)
#
# Requires NSIS (makensis) for the NSIS generator:
#   https://nsis.sourceforge.io/Download   (zlib/libpng licence)
# `cmake --install build/vs2022 --config Release --prefix <dir>` stages the same
# payload without NSIS, which is handy for verifying it.
#
# The installer intentionally shares the host's install DIRECTORY (so plugins/
# lands in the right place) but uses its OWN uninstaller name and Add/Remove
# registry key, so it never clobbers the host's Uninstall.exe or its Add/Remove
# entry. Uninstalling the plugin removes only the plugin's own files.
if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    # Emit the installer inside the build tree, not the caller's CWD.
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/installer")

    set(CPACK_PACKAGE_NAME            "HMV Chartplotter KAP Plugin")
    set(CPACK_PACKAGE_VENDOR          "net.holybee")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "BSB/KAP raster chart plugin for HMV Chartplotter")
    set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(CPACK_PACKAGE_VERSION       "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME     "chartplotter_kap_plugin-${PROJECT_VERSION}-win64")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")

    # Overlay the host: same Program Files subfolder as the host installer, so
    # plugins/ ends up inside the host's directory. The NSIS directory page still
    # lets the user browse elsewhere if their host lives in a non-default path.
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "HMV Chartplotter")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")

    # Distinct identity so the plugin coexists with the host in Add/Remove and
    # does NOT overwrite the host's uninstaller:
    #  - a separate Add/Remove entry (own registry key, else it defaults off the
    #    shared install directory and would collide with the host's key);
    #  - its own uninstaller exe (default "Uninstall.exe" would clobber the
    #    host's, hijacking the host's Add/Remove entry).
    set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "HMV-Chartplotter-KAP-Plugin")
    set(CPACK_NSIS_UNINSTALL_NAME          "Uninstall-KAP-Plugin")
    set(CPACK_NSIS_DISPLAY_NAME "HMV Chartplotter — BSB/KAP Chart Plugin")
    set(CPACK_NSIS_PACKAGE_NAME "HMV Chartplotter KAP Plugin")

    # Clean plugin reinstall (removes a prior plugin install first — its own key,
    # so it never touches the host). No PATH edits; it's just a couple of DLLs.
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    # No Start-menu/desktop shortcuts: a plugin has nothing to launch.

    include(CPack)

    # ---- Auto-build the NSIS installer as part of a Release build ------------
    #
    # Mirrors the host: fold `cpack ... -G NSIS` into the normal build so the
    # installer is produced without a separate command. Gated to MULTI-CONFIG
    # generators (Visual Studio / MSVC) — the CONFIGURATIONS Release install
    # rules above assume that, and CI (Ninja/MinGW, single-config, no makensis)
    # sets no CMAKE_CONFIGURATION_TYPES, so the target is simply absent there.
    # pack_release.cmake no-ops for non-Release configs. Turn off with
    # -DCHARTPLOTTER_KAP_BUILD_INSTALLER=OFF.
    option(CHARTPLOTTER_KAP_BUILD_INSTALLER
        "Auto-build the NSIS installer during a multi-config Release build" ON)
    if(CHARTPLOTTER_KAP_BUILD_INSTALLER AND CMAKE_CONFIGURATION_TYPES)
        add_custom_target(kap_installer ALL
            COMMAND ${CMAKE_COMMAND}
                    -DCFG=$<CONFIG>
                    -DCPACK_CMD=${CMAKE_CPACK_COMMAND}
                    -DCPACK_CONFIG=${CMAKE_BINARY_DIR}/CPackConfig.cmake
                    -DBIN_DIR=${CMAKE_BINARY_DIR}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pack_release.cmake
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Packaging NSIS installer (Release only)"
            VERBATIM
        )
        # Order after the plugin so its DLL is built before cpack stages it.
        add_dependencies(kap_installer kap_plugin)
    endif()
endif()
