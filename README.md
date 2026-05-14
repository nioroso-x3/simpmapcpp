# simplemap

A small C++17 map tile renderer. Fetches XYZ raster tiles, stitches them,
crops to size, and composites overlays on top. Designed to be embedded in
other C++ apps — particularly XR / native apps where bringing in a full web
map stack isn't practical.

Targets Linux and Android (tested on Quest 3 / arm64-v8a).

Made with Claude Opus 4.7.

## What's in the box

- Fractional zoom with sub-pixel center alignment (no snap-to-integer)
- Heading-based rotation with no exposed corners
- DPI-aware output sizes
- "Show N meters of ground" sizing as an alternative to specifying zoom
- Persistent SQLite tile cache with an LRU memory layer on top
- Pluggable HTTP transport via a `TileFetcher` interface
- Overlay layers: icons (PNG or built-in symbols), geographic rectangles and
  circles (sized in meters, rotate with the map), geofences (allowed-area
  shape with hatched forbidden zone, hatches stay screen-aligned)
- RGBA output ready to upload as a GPU texture

What it does NOT do: vector tiles, label placement, route geometry, geocoding,
mouse interaction. This is a renderer, not a map application.

## Quick look

```cpp
#include "HttplibTileFetcher.h"
#include "MapLayer.h"
#include "MapTileRenderer.h"

auto fetcher = std::make_unique<HttplibTileFetcher>();
MapTileRenderer renderer(
    "https://tile.example.com/{z}/{x}/{y}.png",
    std::move(fetcher),
    "/path/to/cache",
    /*disk_mb=*/128,
    /*mem_mb=*/32);

LayerStore layers;

auto pin = std::make_shared<IconLayer>();
pin->latitude  = 40.7128;
pin->longitude = -74.0060;
pin->symbol    = IconSymbol::Cross;
pin->color     = {255, 220, 0, 255};
pin->size_px   = 16;
layers.add(pin);

auto fence = std::make_shared<FenceLayer>();
fence->setCircle(40.7128, -74.0060, /*radius_m=*/120.0);
layers.add(fence);

// 512x512 image showing ~300m of ground, with overlays composited in.
cv::Mat map = renderer.drawMapByArea(
    40.7128, -74.0060,
    /*meters=*/300,
    /*w=*/512, /*h=*/512,
    /*heading=*/0.0, /*dpi=*/96,
    &layers);
```

`map` is a `CV_8UC4` (RGBA) `cv::Mat` ready to hand to a GPU texture. For
saving as a PNG via `cv::imwrite`, convert to BGRA first since OpenCV's
file writers assume BGR ordering:

```cpp
cv::Mat bgra;
cv::cvtColor(map, bgra, cv::COLOR_RGBA2BGRA);
cv::imwrite("map.png", bgra);
```

## Building on Linux

```bash
sudo apt install libopencv-dev libssl-dev cmake build-essential
git clone https://github.com/YOUR_USER/simplemap.git
cd simplemap
cmake -B build
cmake --build build -j
./build/bin/map_example --meters 300 --demo-layers --out demo.png
```

You need to drop two vendored dependencies in place once before the first
build (the repo doesn't commit them):

```bash
curl -L https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.5/httplib.h \
    -o third_party/httplib/httplib.h

curl -L https://sqlite.org/2024/sqlite-amalgamation-3460000.zip -o /tmp/sqlite.zip
unzip -j -o /tmp/sqlite.zip \
    'sqlite-amalgamation-*/sqlite3.c' \
    'sqlite-amalgamation-*/sqlite3.h' \
    -d third_party/sqlite3/
```

See `third_party/*/README.md` for details and version-bump steps.

## Building for Android / Quest 3

```bash
export ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/26.1.10909125
export OpenCV_DIR=/path/to/OpenCV-android-sdk/sdk/native/jni

cmake -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29

cmake --build build-android -j --target simplemap
```

First Android build downloads and cross-compiles OpenSSL 3.5.6 into
`build-android/_deps/openssl-install/`. That takes a few minutes once;
subsequent builds reuse it.

The Linux build uses system OpenSSL via `find_package`. No source build,
no waiting.

## Using as a CMake subdirectory

In your app's CMakeLists:

```cmake
add_subdirectory(third_party/simplemap)
target_link_libraries(your_app PRIVATE simplemap::simplemap)
```

That's it. Include paths, OpenCV, OpenSSL, SQLite, httplib — all transitive.
If your app already calls `find_package(OpenCV)`, do that at the top level
before `add_subdirectory` to avoid duplicate target errors.

When consumed as a subdirectory the example CLI and benchmark binaries are
not built by default. Add `-DSIMPLEMAP_BUILD_EXAMPLE=ON` if you want them.

## The renderer API

```cpp
cv::Mat drawMap(double latitude, double longitude, double zoom,
                int width, int height,
                double heading = 0.0, int dpi = 96,
                const LayerStore* layers = nullptr);

cv::Mat drawMapByArea(double latitude, double longitude, double meters,
                      int width, int height,
                      double heading = 0.0, int dpi = 96,
                      const LayerStore* layers = nullptr);
```

- `zoom` is fractional. 14.7 fetches z=15 tiles and downscales — sharper
  than the alternative of fetching z=14 and upscaling.
- `meters` in `drawMapByArea` is the visible ground extent across the
  smaller of width and height. Computed back to a fractional zoom.
- `heading` rotates the map clockwise in degrees.
- `dpi` scales the output: a 512×512 / 192 DPI call produces a 1024×1024
  image.
- `layers` is optional. Null = no overlays.

Output channel order is **RGBA**, which is what GPU APIs expect. If you
need to save with `cv::imwrite` or hand off to something else that wants
BGRA, convert at the boundary.

## Layers

All layer types share a polymorphic base:

```cpp
class MapLayer {
public:
    double latitude  = 0.0;
    double longitude = 0.0;
    bool   visible   = true;
    std::string id;          // optional, used to look up layers later
};
```

Layers go into a `LayerStore`:

```cpp
LayerStore store;
std::string id = store.add(std::make_shared<IconLayer>(...));
// ...
store.remove(id);
```

`LayerStore` is thread-safe — you can mutate from one thread while a render
is in flight on another. The renderer takes a snapshot at the start of each
`drawMap` call.

### IconLayer

Pixel-sized marker at a lat/lon. Stays the same size regardless of zoom,
stays upright when the map rotates. Either a PNG (pass `cv::Mat` in
`pixel_data`) or one of the built-in symbols: `Square`, `Circle`, `Cross`,
`Triangle`, `Diamond`.

```cpp
auto pin = std::make_shared<IconLayer>();
pin->latitude  = 40.7128;
pin->longitude = -74.0060;
pin->symbol    = IconSymbol::Diamond;
pin->color     = {255, 200, 0, 255};   // RGBA
pin->size_px   = 18;
```

### GeoRectLayer

Rectangle defined in meters, optionally rotated. Rotates with the map.

```cpp
auto r = std::make_shared<GeoRectLayer>();
r->latitude    = 40.7128;
r->longitude   = -74.0060;
r->width_m     = 100.0;
r->height_m    = 60.0;
r->heading_deg = 30.0;       // CW from north
r->fill_color   = {255, 80, 80, 96};
r->stroke_color = {200, 0, 0, 255};
```

### GeoCircleLayer

Circle defined by radius in meters. Rotates with the map.

```cpp
auto c = std::make_shared<GeoCircleLayer>();
c->latitude  = 40.7128;
c->longitude = -74.0060;
c->radius_m  = 40.0;
c->fill_color   = {80, 220, 80, 96};
c->stroke_color = {0, 150, 0, 255};
```

### FenceLayer

Single allowed-area shape with hatched forbidden zone outside. Either a
circle or a polygon; only one fence is rendered per call (additional fences
in the store are ignored).

```cpp
auto fence = std::make_shared<FenceLayer>();
fence->setCircle(40.7128, -74.0060, /*radius_m=*/120.0);

// or:
fence->setPolygon({
    {40.7138, -74.0070},
    {40.7138, -74.0050},
    {40.7118, -74.0050},
    {40.7118, -74.0070},
});
```

The boundary outline rotates with the map (it's geographic). The hatches
themselves are screen-aligned at 45° regardless of map heading.

## Custom HTTP transport

`HttplibTileFetcher` is the default, using cpp-httplib + OpenSSL. To plug in
something else (a different client, a mock for tests, a JNI bridge to a host
HTTP stack) subclass `TileFetcher`:

```cpp
class MyFetcher : public TileFetcher {
public:
    std::vector<uint8_t> fetch(const std::string& url) override { ... }
};
```

Pass `nullptr` for the fetcher to run in **cache-only mode** — useful for
shipping pre-baked tile sets as an app asset, or for environments where you
never want network at runtime.

## Cache

Tiles are persisted in `<cache_dir>/tiles.db` (SQLite). The renderer keeps
a configurable memory LRU on top. Disk and memory limits are constructor
arguments.

The cache dir is a runtime argument — don't default it in cross-platform
code. On Android, pass `app->activity->internalDataPath` or whatever your
platform layer gives you.

## Coordinate accuracy

The library uses Web Mercator throughout. For overlay placement and shape
sizing, the local-tangent-plane (linear) approximation is used — a circle
of radius 100m is drawn as a screen-circle, not as a geodesic ellipse.
This is accurate to within a fraction of a percent at zoom levels >= 10,
which covers any realistic "show me this neighborhood" use case.

At lower zooms or near the poles the linear approximation diverges from
true geodesics. The library doesn't try to be useful for low-zoom
world-scale views — you'd use a different rendering approach for that.

## Performance

A rough sense of the throughput, measured by the included `benchmark`
target — steady-state FPS with warm tile cache, 512×512 output, 10 mixed
overlay layers, 5-second sample:

- Xeon W3-2423 (6c/12t, Linux): ~520 FPS, ~2 ms per frame
- Quest 3 (XR2 Gen 2, native): not measured yet; expect 100-150 FPS

Cost roughly scales with output pixel count. Rotation adds one extra
`warpAffine` pass on already-hot data. Layer count up to ~100 is free;
beyond that you start seeing it.

For typical usage the renderer is far faster than you need. If you call
`drawMap` per-frame on a 90Hz display you're using <2% of a CPU core at
512²; in practice you'd call it on a worker thread at a few Hz when the
view changes.

```bash
./build/bin/benchmark --meters 300 --layers 10 --seconds 5
```

## Tile attribution

You are responsible for complying with the tile provider's terms of service.
The example uses Esri's World Imagery, which requires attribution overlay
when displayed. For unrestricted-ish use look at OpenStreetMap (with their
tile usage policy) or self-hosted tile servers.

## Dependencies

| Library          | How it's handled                                       |
|------------------|--------------------------------------------------------|
| OpenCV (core, imgcodecs, imgproc) | System (Linux) / SDK (Android)        |
| OpenSSL          | System (Linux) / built-by-cmake (Android)              |
| cpp-httplib      | Vendored in `third_party/httplib/`                     |
| SQLite           | Vendored in `third_party/sqlite3/`                     |

OpenCV is by far the largest dependency. If you're size-sensitive on
Android, rebuild OpenCV from source with only the three modules listed
above. Out of the box the Android SDK is ~30 MB per ABI; trimmed, more
like 5 MB.

## Files

```
.
├── CMakeLists.txt
├── README.md
├── LICENSE
├── cmake/
│   └── OpenSSLDep.cmake          # system or ExternalProject_Add
├── include/                       # public headers
│   ├── HttplibTileFetcher.h
│   ├── MapLayer.h
│   ├── MapTileRenderer.h
│   └── TileFetcher.h
├── src/
│   ├── HttplibTileFetcher.cpp
│   ├── LayerRenderer.{h,cpp}     # internal; not a public API
│   ├── MapLayer.cpp
│   └── MapTileRenderer.cpp
├── example/
│   └── main.cpp                  # map_example CLI
├── benchmark/
│   └── main.cpp                  # FPS benchmark
└── third_party/                  # populated by user, see READMEs
    ├── httplib/
    └── sqlite3/
```

## License

MIT — see `LICENSE`. Vendored dependencies keep their original licenses
(cpp-httplib: MIT, SQLite: public domain).
