#pragma once

#include "TileFetcher.h"

#include <list>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <sqlite3.h>
#include <string>
#include <unordered_map>

class LayerStore;  // forward decl, defined in MapLayer.h

class MapTileRenderer {
public:
    // The renderer takes ownership of the fetcher. Pass nullptr for cache-only
    // mode (useful when shipping a pre-baked tile cache as an app asset).
    MapTileRenderer(const std::string& tileset_url,
                    std::unique_ptr<TileFetcher> fetcher,
                    const std::string& cache_dir = "",
                    size_t max_disk_cache_mb = 128,
                    size_t max_memory_cache_mb = 32);
    ~MapTileRenderer();

    // Render a map centered at (latitude, longitude) at the given fractional
    // zoom. Returns an RGBA (CV_8UC4) image of width*dpi/96 by height*dpi/96.
    // If `layers` is non-null, the layers it owns are composited onto the
    // output.
    cv::Mat drawMap(double latitude, double longitude, double zoom,
                    int width, int height, double heading = 0.0, int dpi = 96,
                    const LayerStore* layers = nullptr);

    // Render a map sized so that `meters` of ground is visible across the
    // smaller of (width, height). Best at zoom levels >= 10 where Web Mercator
    // distortion across the frame is negligible.
    cv::Mat drawMapByArea(double latitude, double longitude, double meters,
                          int width, int height, double heading = 0.0,
                          int dpi = 96,
                          const LayerStore* layers = nullptr);

private:
    struct TileCoord { int x, y, z; };
    struct BoundingBox { double min_lat, max_lat, min_lon, max_lon; };

    struct TileKey {
        int x, y, z;
        bool operator==(const TileKey& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct TileKeyHash {
        std::size_t operator()(const TileKey& k) const {
            std::size_t h = std::hash<int>()(k.x);
            h ^= std::hash<int>()(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::string tileset_url_;
    std::unique_ptr<TileFetcher> fetcher_;
    std::string cache_dir_;
    sqlite3* db_;
    size_t max_disk_cache_bytes_;
    size_t max_memory_cache_bytes_;
    static constexpr int TILE_SIZE = 256;

    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<TileKey, cv::Mat, TileKeyHash> memory_cache_;
    mutable std::list<TileKey> lru_list_;
    mutable std::unordered_map<TileKey, std::list<TileKey>::iterator, TileKeyHash> lru_map_;
    mutable size_t current_memory_usage_;

    TileCoord latLonToTile(double lat, double lon, int zoom) const;
    void latLonToPixel(double lat, double lon, int zoom, double& px, double& py) const;
    BoundingBox calculateBounds(double lat, double lon, double zoom,
                                int width, int height, int dpi) const;
    std::vector<TileCoord> getTilesInBounds(const BoundingBox& bounds, int zoom) const;

    cv::Mat downloadTile(const TileCoord& coord) const;
    std::string buildTileUrl(const TileCoord& coord) const;

    cv::Mat stitchAndCenter(const std::vector<TileCoord>& tiles,
                            int tile_zoom, double scale_factor,
                            double center_lat, double center_lon,
                            int out_w, int out_h) const;

    bool initializeCache();
    void cleanupCache();
    cv::Mat getTileFromCache(const TileCoord& coord) const;
    void putTileInCache(const TileCoord& coord, const cv::Mat& tile) const;
    cv::Mat loadTileFromDisk(const TileCoord& coord) const;
    void saveTileToDisk(const TileCoord& coord, const cv::Mat& tile) const;
    void evictMemoryCache() const;
    void evictDiskCache() const;
    size_t estimateTileSize(const cv::Mat& tile) const;
    TileKey coordToKey(const TileCoord& coord) const;
};
