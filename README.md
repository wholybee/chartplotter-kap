# BSB/KAP raster chart plugin for HMV Chartplotter

Reads **BSB/KAP 2.0** raster charts (the format NOAA, imgkap, OpenCPN and most
chart converters produce) and serves them to HMV Chartplotter's raster chart
layer as map tiles — alongside the host's built-in MBTiles charts, beneath the
ENC vector cells.

Loaded at runtime via `QPluginLoader`. It links **only Qt6** and the host's
plugin-SDK headers (vendored in [`sdk/`](sdk/)) — no GDAL, no libbsb, and no host
code.

## What it does

The plugin implements the host's `IRasterChartSource` interface (plugin ABI v5):

| step | what happens |
|---|---|
| `canHandle` | Does the chart folder contain any `*.kap`? (first hit wins; nothing is parsed) |
| `catalog` | Parse every KAP's *header* — cheap, no imagery — into a chart list with coverage + zoom range |
| `tile` | Render one XYZ tile on demand, decoding only the raster rows that tile covers |

Serving tiles rather than whole images is what lets the host's existing raster
machinery — the pyramid, the pixmap LRU, coarser-ancestor fallback while tiles
load, the painter blit and the GPU textured-quad path — apply to KAP charts with
no special cases.

Because a KAP's row-offset index table makes every raster row individually
addressable, a tile decodes **only its own rows**. Nothing ever holds a whole
decoded chart in memory, so a folder of a thousand 2048×6144 charts costs no more
than the tiles currently on screen.

## Requirements & limitations

- **Mercator charts, including skewed (rotated) ones.** `KNP/PR` must be
  `MERCATOR`; the skew angle (`KNP/SK`) may be anything. NOAA rotates most of its
  approach and harbour charts to run a channel or coastline up the page, so this
  matters — nearly half of a NOAA RNC set is skewed. The georeference is a
  least-squares **affine** fit of the `REF` control points from raster pixels to
  projected Mercator metres, which places a rotated chart exactly and collapses to
  the north-up case on its own when the chart isn't rotated.
- **Non-Mercator projections are declined**, not drawn. `POLYCONIC` and the like
  need a different model, and fitting a Mercator affine to one would place it in
  visibly the wrong position — a chart drawn in the wrong place is worse than a
  chart not drawn. Declined charts are skipped individually; one never sinks the
  folder.
- **Charts crossing the 180° antimeridian are not handled.** The georeference is
  a linear fit of longitude against x, and `REF` points either side of the seam
  (179 → −179) are discontinuous, so the fit would be wrong. Untested — no such
  chart was available. Everything else in the pipeline (the host wraps the scene
  frame by whole world-widths) is seam-safe; only this fit is not.
- Raster depths 0–7 are supported, including the degenerate depth-0 charts that
  `imgkap` emits for a tile of one solid colour (open ocean).
- `PLY/` coverage polygons are honoured, so a scanned chart's paper collar and
  legend are clipped away and neighbouring charts quilt cleanly. The test is
  skipped entirely when the polygon covers the whole image (the usual case for
  tiled mosaics), so it costs nothing there.

## Building

Needs Qt 6 and CMake ≥ 3.21. The plugin must be built with a C++ ABI matching the
host you load it into (on Windows: MSVC host ⇒ MSVC plugin).

```sh
cmake --preset vs2022                       # or: cmake -S . -B build -G Ninja
cmake --build build/vs2022 --config Release
```

To drop the module straight into an installed host, point the build at it:

```sh
cmake --preset vs2022 -DCHARTPLOTTER_APP_DIR="C:/path/to/app"   # folder with hmvchartplotter.exe
```

…which copies `chartplotter_kap_plugin.dll` into that app's `plugins/`. Otherwise
copy it there by hand; the host scans `plugins/` next to the executable
(`Contents/PlugIns` on macOS).

## Windows installer (NSIS)

A multi-config **MSVC/Visual Studio** Release build also produces an NSIS
installer, the same way the host application project does — no extra command:

```sh
cmake --preset vs2022
cmake --build build/vs2022 --config Release
# -> build/vs2022/installer/chartplotter_kap_plugin-<version>-win64.exe
```

It needs [NSIS](https://nsis.sourceforge.io/Download) (`makensis`) installed. The
installer overlays an existing HMV Chartplotter installation: it drops
`chartplotter_kap_plugin.dll` into `<host>\plugins\`, so the host loads it on next
launch. The directory page defaults to the host's default location
(`%ProgramFiles%\HMV Chartplotter`) and lets you browse elsewhere if your host
lives somewhere else.

It coexists cleanly with the host: its own Add/Remove Programs entry, its own
uninstaller (`Uninstall-KAP-Plugin.exe`, so it never overwrites the host's
`Uninstall.exe`), and it removes only its own file on uninstall.

**Qt DLLs are intentionally not bundled.** The host is a Qt application and
already ships `Qt6Core.dll` / `Qt6Gui.dll` (and the rest of the Qt runtime) next
to its exe — this plugin links nothing the host doesn't already provide. Shipping
duplicates would be redundant, and worse: the plugin's uninstaller would then
delete Qt DLLs the *host* depends on. For a host-less / self-contained layout,
configure with `-DCHARTPLOTTER_KAP_INSTALLER_BUNDLE_QT=ON` to place `Qt6Core.dll`
and `Qt6Gui.dll` in the install root as well. (This is separate from the rare case
of a plugin needing a Qt module the host lacks — for that, list its DLL in
`CHARTPLOTTER_KAP_EXTRA_RUNTIME_DLLS`.)

The installer target is created only for multi-config generators (Visual Studio);
the single-config MinGW/Ninja CI build does not build it (no `makensis` there),
exactly as in the host project. Disable it with
`-DCHARTPLOTTER_KAP_BUILD_INSTALLER=OFF`.

To stage the same payload without NSIS (to inspect it):

```sh
cmake --install build/vs2022 --config Release --prefix some/dir
```

## Smoke test

`test_kap` exercises the reader against a real chart folder with no GUI and no
host:

```sh
build/vs2022/Release/test_kap.exe "C:/path/to/KAP/folder" [outdir]
```

It parses every chart, checks each georeference round-trips and that native zoom
reproduces the raster's true pixel scale, then writes two PNGs:

- **`kap_mosaic.png`** — every chart in the folder composited into one image *by
  geography*. This is the real test: the charts only assemble into a coherent
  coastline if each one's georeference is independently correct, so a good run is
  recognisable at a glance and a bad one is obvious.
- **`kap_native.png`** — one tile at native zoom from the most detailed chart in
  the set, to eyeball decode quality 1:1.

Exits non-zero if any check fails.

## Chart sources

Any BSB/KAP set works. The samples this was developed against are OpenCPN-style
KAP mosaics generated from OSM tiles (`imgkap`), which are exact Web-Mercator tile
mosaics — for those, a native-zoom tile is a **lossless** copy of the chart's own
pixels.

## Licence

GPL-2.0-or-later — see [COPYING](COPYING). The vendored SDK headers in `sdk/`
belong to the host application and are LGPL-2.1; their own notices apply. The BSB
decoding here is an independent implementation written from the published format
description and verified against real charts.
