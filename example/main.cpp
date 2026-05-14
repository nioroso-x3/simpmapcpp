#include "HttplibTileFetcher.h"
#include "MapLayer.h"
#include "MapTileRenderer.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <string>

void printUsage() {
    std::cout << "Usage: map_example [options]\n"
              << "Options:\n"
              << "  --lat <value>     Latitude (default: 40.7128)\n"
              << "  --lon <value>     Longitude (default: -74.0060)\n"
              << "  --zoom <value>    Zoom level, supports fractional (default: 12.0)\n"
              << "  --meters <value>  Visible ground meters across shorter dimension.\n"
              << "  --width <value>   Width in pixels (default: 512)\n"
              << "  --height <value>  Height in pixels (default: 512)\n"
              << "  --heading <value> Rotation degrees (default: 0.0)\n"
              << "  --dpi <value>     DPI (default: 96)\n"
              << "  --cache <dir>     Cache directory (default: ./cache)\n"
              << "  --out <file>      Output PNG (default: output_map.png)\n"
              << "  --demo-layers     Add demo overlays (circle, rectangle, icon at center)\n"
              << "  --help            Show this help\n";
}

int main(int argc, char* argv[]) {
    try {
        std::string tileset_url =
            "https://services.arcgisonline.com/ArcGIS/rest/services/"
            "World_Imagery/MapServer/tile/{z}/{y}/{x}";
        double latitude = 40.7128, longitude = -74.0060;
        double zoom = 12.0, meters = -1.0;
        bool zoom_set = false, demo_layers = false;
        int width = 512, height = 512, dpi = 96;
        double heading = 0.0;
        std::string cache_dir = "./cache";
        std::string out_path = "output_map.png";

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--lat") == 0 && i + 1 < argc) latitude = std::stod(argv[++i]);
            else if (strcmp(argv[i], "--lon") == 0 && i + 1 < argc) longitude = std::stod(argv[++i]);
            else if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) { zoom = std::stod(argv[++i]); zoom_set = true; }
            else if (strcmp(argv[i], "--meters") == 0 && i + 1 < argc) meters = std::stod(argv[++i]);
            else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width = std::stoi(argv[++i]);
            else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) height = std::stoi(argv[++i]);
            else if (strcmp(argv[i], "--heading") == 0 && i + 1 < argc) heading = std::stod(argv[++i]);
            else if (strcmp(argv[i], "--dpi") == 0 && i + 1 < argc) dpi = std::stoi(argv[++i]);
            else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) cache_dir = argv[++i];
            else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
            else if (strcmp(argv[i], "--demo-layers") == 0) demo_layers = true;
            else if (strcmp(argv[i], "--help") == 0) { printUsage(); return 0; }
            else { std::cerr << "Unknown option: " << argv[i] << "\n"; printUsage(); return 1; }
        }

        if (meters > 0.0 && zoom_set) {
            std::cerr << "Specify either --zoom or --meters, not both\n";
            return 1;
        }

        auto fetcher = std::make_unique<HttplibTileFetcher>();
        MapTileRenderer renderer(tileset_url, std::move(fetcher), cache_dir, 128, 32);

        LayerStore layers;
        if (demo_layers) {
            // Translucent red rectangle, 100m x 60m, rotated 30 degrees,
            // centered on the map
            auto rect = std::make_shared<GeoRectLayer>();
            rect->latitude = 40.7128;
            rect->longitude = -74.006;
            rect->width_m = 100.0;
            rect->height_m = 60.0;
            rect->heading_deg = 30.0;
            rect->fill_color   = {255, 80, 80, 96};
            rect->stroke_color = {200, 0, 0, 255};
            rect->stroke_width_px = 2;
            layers.add(rect);

            // Translucent green circle, 40m radius
            auto circle = std::make_shared<GeoCircleLayer>();
            circle->latitude = 40.714;
            circle->longitude = -74.006;
            circle->radius_m = 40.0;
            circle->fill_color   = {80, 220, 80, 96};
            circle->stroke_color = {0, 150, 0, 255};
            circle->stroke_width_px = 2;
            layers.add(circle);

            // Yellow X icon at the center
            auto pin = std::make_shared<IconLayer>();
            pin->latitude = 40.7129;
            pin->longitude = -74.008;
            pin->symbol = IconSymbol::Cross;
            pin->color = {255, 220, 0, 255};
            pin->size_px = 18;
            pin->stroke_width_px = 3;
            layers.add(pin);

            // Geofence: circular allowed zone, 120m radius.
            // Everything outside is hatched in purple.
            auto fence = std::make_shared<FenceLayer>();
            fence->setCircle(40.7128, -74.006, 800.0);
            layers.add(fence);
        }

        std::cout << "Rendering at " << latitude << ", " << longitude << "\n";
        cv::Mat map = (meters > 0.0)
            ? renderer.drawMapByArea(latitude, longitude, meters, width, height, heading, dpi, &layers)
            : renderer.drawMap(latitude, longitude, zoom, width, height, heading, dpi, &layers);

        if (map.empty()) { std::cerr << "Failed to generate map\n"; return 1; }
        std::cout << "Output size: " << map.cols << "x" << map.rows << "\n";

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
