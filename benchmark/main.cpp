// Steady-state FPS benchmark for simplemap.
//
// Pre-warms the tile cache, then renders repeatedly and reports FPS.
// Reflects the rendering hot path (cache hit, decode-from-RGBA, layer
// composite, rotate, crop) without network noise.
//
// Usage:
//   benchmark [--lat L] [--lon L] [--zoom Z] [--meters M]
//             [--width W] [--height H] [--heading H] [--dpi D]
//             [--layers N] [--seconds S] [--cache DIR]

#include "HttplibTileFetcher.h"
#include "MapLayer.h"
#include "MapTileRenderer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

void populateLayers(LayerStore& store, int n, double lat, double lon) {
    if (n <= 0) return;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-0.0005, 0.0005);
    std::uniform_real_distribution<double> size_m(20.0, 80.0);
    std::uniform_real_distribution<double> heading(0.0, 360.0);
    std::uniform_int_distribution<int>     kind(0, 2);
    std::uniform_int_distribution<int>     sym(0, 4);

    for (int i = 0; i < n; ++i) {
        double la = lat + jitter(rng);
        double lo = lon + jitter(rng);
        switch (kind(rng)) {
            case 0: {
                auto r = std::make_shared<GeoRectLayer>();
                r->latitude = la; r->longitude = lo;
                r->width_m = size_m(rng); r->height_m = size_m(rng);
                r->heading_deg = heading(rng);
                r->fill_color = {255, 80, 80, 96};
                r->stroke_color = {200, 0, 0, 255};
                store.add(r);
                break;
            }
            case 1: {
                auto c = std::make_shared<GeoCircleLayer>();
                c->latitude = la; c->longitude = lo;
                c->radius_m = size_m(rng) * 0.5;
                c->fill_color = {80, 220, 80, 96};
                c->stroke_color = {0, 150, 0, 255};
                store.add(c);
                break;
            }
            default: {
                auto p = std::make_shared<IconLayer>();
                p->latitude = la; p->longitude = lo;
                p->symbol = static_cast<IconSymbol>(sym(rng));
                p->color = {255, 220, 0, 255};
                p->size_px = 16;
                store.add(p);
                break;
            }
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* tileset_url =
        "https://services.arcgisonline.com/ArcGIS/rest/services/"
        "World_Imagery/MapServer/tile/{z}/{y}/{x}";

    double lat = 40.7128, lon = -74.0060;
    double zoom = 17.0, meters = -1.0;
    bool zoom_set = false;
    int width = 512, height = 512, dpi = 96;
    double heading = 0.0;
    int layer_count = 10;
    double duration_s = 5.0;
    std::string cache_dir = "./cache_bench";

    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* s) { return strcmp(argv[i], s) == 0; };
        if      (eq("--lat")     && i + 1 < argc) lat = std::stod(argv[++i]);
        else if (eq("--lon")     && i + 1 < argc) lon = std::stod(argv[++i]);
        else if (eq("--zoom")    && i + 1 < argc) { zoom = std::stod(argv[++i]); zoom_set = true; }
        else if (eq("--meters")  && i + 1 < argc) meters = std::stod(argv[++i]);
        else if (eq("--width")   && i + 1 < argc) width = std::stoi(argv[++i]);
        else if (eq("--height")  && i + 1 < argc) height = std::stoi(argv[++i]);
        else if (eq("--heading") && i + 1 < argc) heading = std::stod(argv[++i]);
        else if (eq("--dpi")     && i + 1 < argc) dpi = std::stoi(argv[++i]);
        else if (eq("--layers")  && i + 1 < argc) layer_count = std::stoi(argv[++i]);
        else if (eq("--seconds") && i + 1 < argc) duration_s = std::stod(argv[++i]);
        else if (eq("--cache")   && i + 1 < argc) cache_dir = argv[++i];
        else if (eq("--help")) {
            std::cout << "Usage: benchmark [--lat L] [--lon L] [--zoom Z | --meters M]\n"
                         "                 [--width W] [--height H] [--heading H] [--dpi D]\n"
                         "                 [--layers N] [--seconds S] [--cache DIR]\n";
            return 0;
        }
        else { std::cerr << "Unknown option: " << argv[i] << "\n"; return 1; }
    }

    if (meters > 0.0 && zoom_set) {
        std::cerr << "Specify either --zoom or --meters, not both\n";
        return 1;
    }

    auto fetcher = std::make_unique<HttplibTileFetcher>();
    MapTileRenderer renderer(tileset_url, std::move(fetcher), cache_dir, 256, 64);
    LayerStore layers;
    populateLayers(layers, layer_count, lat, lon);

    auto render = [&]() {
        return (meters > 0.0)
            ? renderer.drawMapByArea(lat, lon, meters, width, height, heading, dpi, &layers)
            : renderer.drawMap(lat, lon, zoom, width, height, heading, dpi, &layers);
    };

    std::cout << "Warming cache...\n";
    auto warm_start = Clock::now();
    cv::Mat first = render();
    double warm_ms = std::chrono::duration<double, std::milli>(Clock::now() - warm_start).count();
    if (first.empty()) {
        std::cerr << "First render failed; can't benchmark\n";
        return 1;
    }
    std::cout << "First render (cold): " << warm_ms << " ms\n";

    // One more warmup to make sure everything is hot (memory cache, OS page
    // cache for the SQLite db, OpenCV's internal lazy state, etc.).
    render();

    std::cout << "Benchmarking for " << duration_s << "s "
              << "(" << width << "x" << height
              << ", zoom=" << zoom
              << ", heading=" << heading
              << ", dpi=" << dpi
              << ", layers=" << layer_count << ")...\n";

    auto bench_start = Clock::now();
    auto bench_end = bench_start + std::chrono::duration_cast<Clock::duration>(Seconds(duration_s));
    long frames = 0;
    double min_ms = 1e9, max_ms = 0;

    while (Clock::now() < bench_end) {
        auto t0 = Clock::now();
        cv::Mat m = render();
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (ms < min_ms) min_ms = ms;
        if (ms > max_ms) max_ms = ms;
        ++frames;
        // Force the result to be used so the optimizer doesn't elide work.
        if (m.empty()) { std::cerr << "Render returned empty\n"; return 1; }
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - bench_start).count();
    double fps = frames / elapsed;
    double avg_ms = (elapsed * 1000.0) / frames;

    std::cout << "\n";
    std::cout << "Frames:   " << frames << "\n";
    std::cout << "Elapsed:  " << elapsed << " s\n";
    std::cout << "FPS:      " << fps << "\n";
    std::cout << "avg/frame: " << avg_ms << " ms\n";
    std::cout << "min/max:  " << min_ms << " / " << max_ms << " ms\n";
    return 0;
}
