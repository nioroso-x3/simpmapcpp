#include "HttplibTileFetcher.h"
#include "MapLayer.h"
#include "MapTileRenderer.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <thread>

void printUsage() {
    std::cout << "Usage: map_example [options]\n"
              << "Options:\n"
              << "  --lat <value>     Latitude (default: 40.7128)\n"
              << "  --lon <value>     Longitude (default: -74.0060)\n"
              << "  --zoom <value>    Zoom level, supports fractional (default: 12.0)\n"
              << "  --meters <value>  Visible ground meters across shorter dimension\n"
              << "  --width <value>   Width in pixels (default: 512)\n"
              << "  --height <value>  Height in pixels (default: 512)\n"
              << "  --heading <value> Rotation degrees (default: 0.0)\n"
              << "  --dpi <value>     DPI (default: 96)\n"
              << "  --cache <dir>     Cache directory (default: ./cache)\n"
              << "  --out <file>      Output PNG (default: output_map.png)\n"
              << "  --demo-layers     Add demo overlays (rect, circle, icon, fence)\n"
              << "  --async           Enable background tile loading with placeholders\n"
              << "                    (call again later to see filled-in tiles)\n"
              << "  --help            Show this help\n";
}

int main(int argc, char* argv[]) {
    try {
        std::string tileset_url =
            "https://services.arcgisonline.com/ArcGIS/rest/services/"
            "World_Imagery/MapServer/tile/{z}/{y}/{x}";

        double latitude = 40.7128, longitude = -74.0060;
        double zoom = 12.0, meters = -1.0;
        bool   zoom_set = false, demo_layers = false, async = false;
        int    width = 512, height = 512, dpi = 96;
        double heading = 0.0;
        std::string cache_dir = "./cache";
        std::string out_path  = "output_map.png";

        for (int i = 1; i < argc; ++i) {
            auto eq = [&](const char* s) { return strcmp(argv[i], s) == 0; };
            if      (eq("--lat")     && i + 1 < argc) latitude = std::stod(argv[++i]);
            else if (eq("--lon")     && i + 1 < argc) longitude = std::stod(argv[++i]);
            else if (eq("--zoom")    && i + 1 < argc) { zoom = std::stod(argv[++i]); zoom_set = true; }
            else if (eq("--meters")  && i + 1 < argc) meters = std::stod(argv[++i]);
            else if (eq("--width")   && i + 1 < argc) width  = std::stoi(argv[++i]);
            else if (eq("--height")  && i + 1 < argc) height = std::stoi(argv[++i]);
            else if (eq("--heading") && i + 1 < argc) heading = std::stod(argv[++i]);
            else if (eq("--dpi")     && i + 1 < argc) dpi = std::stoi(argv[++i]);
            else if (eq("--cache")   && i + 1 < argc) cache_dir = argv[++i];
            else if (eq("--out")     && i + 1 < argc) out_path = argv[++i];
            else if (eq("--demo-layers")) demo_layers = true;
            else if (eq("--async"))       async = true;
            else if (eq("--help"))      { printUsage(); return 0; }
            else { std::cerr << "Unknown option: " << argv[i] << "\n"; printUsage(); return 1; }
        }

        if (meters > 0.0 && zoom_set) {
            std::cerr << "Specify either --zoom or --meters, not both\n";
            return 1;
        }

        auto fetcher = std::make_unique<HttplibTileFetcher>();
        MapTileRenderer renderer(tileset_url, std::move(fetcher), cache_dir, 128, 32);
        if (async) renderer.enableAsyncLoading(4);

        LayerStore layers;
        if (demo_layers) {
            // Translucent red rectangle, 100m x 60m, rotated 30 degrees,
            // centered on the map.
            auto rect = std::make_shared<GeoRectLayer>();
            rect->latitude = latitude;
            rect->longitude = longitude;
            rect->width_m = 100.0;
            rect->height_m = 60.0;
            rect->heading_deg = 30.0;
            rect->fill_color   = {255, 80, 80, 96};
            rect->stroke_color = {200, 0, 0, 255};
            rect->stroke_width_px = 2;
            layers.add(rect);

            // Translucent green circle, 40m radius.
            auto circle = std::make_shared<GeoCircleLayer>();
            circle->latitude = latitude;
            circle->longitude = longitude;
            circle->radius_m = 40.0;
            circle->fill_color   = {80, 220, 80, 96};
            circle->stroke_color = {0, 150, 0, 255};
            circle->stroke_width_px = 2;
            layers.add(circle);

            // Yellow X icon at center.
            auto pin = std::make_shared<IconLayer>();
            pin->latitude = latitude;
            pin->longitude = longitude;
            pin->symbol = IconSymbol::Cross;
            pin->color = {255, 220, 0, 255};
            pin->size_px = 18;
            pin->stroke_width_px = 3;
            layers.add(pin);

            // Circular geofence, 120m allowed radius. Outside is hatched.
            auto fence = std::make_shared<FenceLayer>();
            fence->setCircle(latitude, longitude, 120.0);
            layers.add(fence);
        }

        std::cout << "Rendering at " << latitude << ", " << longitude;
        if (async) std::cout << " (async mode)";
        std::cout << "\n";

        cv::Mat map;
        if (async && (meters > 0.0 || zoom_set || true)) {
            // Async mode only makes sense if we render repeatedly: the first
            // call returns placeholders and queues background fetches, later
            // calls pick up completed tiles. Simulate an interactive loop:
            // render, wait, render again, until the image stabilizes or we
            // hit a time budget.
            const int    max_iters = 60;        // ~6s at 100ms/iter
            const int    settle_iters = 3;      // stop after N identical frames
            int          stable = 0;
            cv::Mat      prev;
            for (int it = 0; it < max_iters; ++it) {
                map = (meters > 0.0)
                    ? renderer.drawMapByArea(latitude, longitude, meters, width, height, heading, dpi, &layers)
                    : renderer.drawMap(latitude, longitude, zoom, width, height, heading, dpi, &layers);
                if (!prev.empty() && map.size() == prev.size()) {
                    cv::Mat diff;
                    cv::absdiff(map, prev, diff);
                    if (cv::countNonZero(diff.reshape(1)) == 0) {
                        if (++stable >= settle_iters) break;
                    } else {
                        stable = 0;
                    }
                }
                map.copyTo(prev);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::cout << "Async render settled.\n";
        } else {
            map = (meters > 0.0)
                ? renderer.drawMapByArea(latitude, longitude, meters, width, height, heading, dpi, &layers)
                : renderer.drawMap(latitude, longitude, zoom, width, height, heading, dpi, &layers);
        }

        if (map.empty()) { std::cerr << "Failed to generate map\n"; return 1; }
        std::cout << "Output size: " << map.cols << "x" << map.rows << "\n";

        if (async) {
            std::cout << "Note: in async mode missing tiles render as grey "
                         "placeholders. Run again to see fetched tiles.\n";
        }

        // Renderer outputs RGBA (for GPU upload); cv::imwrite expects BGR/BGRA.
        cv::Mat bgra;
        cv::cvtColor(map, bgra, cv::COLOR_RGBA2BGRA);
        cv::imwrite(out_path, bgra);
        std::cout << "Wrote " << out_path << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
