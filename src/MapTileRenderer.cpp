#include "MapTileRenderer.h"
#include "AsyncTileLoader.h"
#include "MapLayer.h"
#include "LayerRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#if defined(__ANDROID__)
  #include <android/log.h>
  #define LOG_TAG "MapTileRenderer"
  #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
  #define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
  #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
  #define LOGD(...) do { std::fprintf(stderr, "[DEBUG] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while(0)
  #define LOGW(...) do { std::fprintf(stderr, "[WARN] " __VA_ARGS__);  std::fprintf(stderr, "\n"); } while(0)
  #define LOGE(...) do { std::fprintf(stderr, "[ERROR] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while(0)
#endif

namespace {

bool create_directories(const std::string& path) {
    if (path.empty()) return true;
    std::string current;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!current.empty() && current != ".") {
                struct stat st;
                if (stat(current.c_str(), &st) != 0) {
                    if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                        return false;
                    }
                } else if (!S_ISDIR(st.st_mode)) {
                    return false;
                }
            }
        }
        if (i < path.size()) current += path[i];
    }
    return true;
}

}  // namespace

MapTileRenderer::MapTileRenderer(const std::string& tileset_url,
                                 std::unique_ptr<TileFetcher> fetcher,
                                 const std::string& cache_dir,
                                 size_t max_disk_cache_mb,
                                 size_t max_memory_cache_mb)
    : tileset_url_(tileset_url),
      fetcher_(std::move(fetcher)),
      cache_dir_(cache_dir),
      db_(nullptr),
      max_disk_cache_bytes_(max_disk_cache_mb * 1024 * 1024),
      max_memory_cache_bytes_(max_memory_cache_mb * 1024 * 1024),
      current_memory_usage_(0) {
    if (!cache_dir_.empty()) initializeCache();
}

MapTileRenderer::~MapTileRenderer() { cleanupCache(); }

void MapTileRenderer::enableAsyncLoading(int num_threads) {
    if (async_loader_) return;          // idempotent
    if (!fetcher_) return;              // no transport, nothing to do

    // Build the placeholder once: solid mid-grey, opaque, RGBA so it
    // composites cleanly with the rest of the pipeline.
    placeholder_tile_ = cv::Mat(TILE_SIZE, TILE_SIZE, CV_8UC4,
                                cv::Scalar(170, 170, 170, 255));

    // Callback writes the completed tile into the renderer's normal cache.
    // The next drawMap call will pick it up as a cache hit. Capturing
    // `this` is safe because the loader is a member and is destroyed
    // before `this` becomes invalid.
    auto on_complete = [this](const AsyncTileLoader::TileKey& k,
                              const cv::Mat& tile) {
        TileCoord c{k.x, k.y, k.z};
        putTileInCache(c, tile);
    };

    async_loader_ = std::make_unique<AsyncTileLoader>(
        std::move(fetcher_), std::move(on_complete), num_threads);
}

cv::Mat MapTileRenderer::drawMap(double latitude, double longitude, double zoom,
                                 int width, int height, double heading, int dpi,
                                 const LayerStore* layers) {
    //LOGD("drawMap zoom=%.3f", zoom);

    int tile_zoom = (zoom == std::floor(zoom))
                        ? static_cast<int>(zoom)
                        : static_cast<int>(std::ceil(zoom));
    tile_zoom = std::max(0, std::min(22, tile_zoom));
    double scale_factor = std::pow(2.0, zoom - tile_zoom);

    int diag = static_cast<int>(std::ceil(
        std::sqrt(double(width) * width + double(height) * height)));
    double dpi_scale = dpi / 96.0;
    int canvas = static_cast<int>(std::ceil(diag * dpi_scale));

    BoundingBox bounds = calculateBounds(latitude, longitude, zoom,
                                         canvas, canvas, 96);
    auto tiles = getTilesInBounds(bounds, tile_zoom);
    //LOGD("Fetching %zu tiles", tiles.size());

    cv::Mat oversized = stitchAndCenter(tiles, tile_zoom, scale_factor,
                                        latitude, longitude, canvas, canvas);

    // Snapshot layers once so add/remove from other threads can't race us.
    std::vector<std::shared_ptr<MapLayer>> layer_snapshot;
    LayerRenderer::CanvasGeometry geom{};
    if (layers) {
        layer_snapshot = layers->snapshot();
        // Meters per pixel of the oversized canvas. The canvas pixels are at
        // the fractional zoom's native pixel density — dpi_scale only changes
        // canvas size (headroom for rotation), not pixel resolution.
        geom.center_lat = latitude;
        geom.center_lon = longitude;
        geom.center_px = oversized.cols * 0.5;
        geom.center_py = oversized.rows * 0.5;
        geom.meters_per_pixel =
            156543.034 * std::cos(latitude * M_PI / 180.0) / std::pow(2.0, zoom);
        geom.canvas_w = oversized.cols;
        geom.canvas_h = oversized.rows;
        /*LOGD("Layer geom: mpp=%.3f canvas=%dx%d center_px=(%.1f,%.1f) layers=%zu",
             geom.meters_per_pixel, geom.canvas_w, geom.canvas_h,
             geom.center_px, geom.center_py, layer_snapshot.size());
        */
        // Geographic layers go onto the pre-rotation canvas so they rotate
        // with the map.
        LayerRenderer::drawGeographicLayers(oversized, layer_snapshot, geom);
    }

    // Rotate, then crop. This was previously applyRotationAndDPI but we
    // need to capture the crop offset so icons can be placed correctly.
    cv::Mat rotated;
    if (std::abs(heading) > 0.01) {
        cv::Point2f rot_center(oversized.cols / 2.0f, oversized.rows / 2.0f);
        cv::Mat M = cv::getRotationMatrix2D(rot_center, heading, 1.0);
        cv::warpAffine(oversized, rotated, M, oversized.size(),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                       cv::Scalar(0, 0, 0, 0));
    } else {
        rotated = oversized;
    }

    int final_w = static_cast<int>(std::round(width  * dpi_scale));
    int final_h = static_cast<int>(std::round(height * dpi_scale));
    int crop_x = std::max(0, (rotated.cols - final_w) / 2);
    int crop_y = std::max(0, (rotated.rows - final_h) / 2);
    int crop_w = std::min(final_w, rotated.cols - crop_x);
    int crop_h = std::min(final_h, rotated.rows - crop_y);
    cv::Mat output = rotated(cv::Rect(crop_x, crop_y, crop_w, crop_h)).clone();

    // Fence hatches: screen-aligned 45° lines drawn in output space (so
    // they don't rotate with the map). Done before icons so icons sit
    // on top.
    if (layers && !layer_snapshot.empty()) {
        LayerRenderer::drawFenceHatches(output, layer_snapshot, geom,
                                        heading, crop_x, crop_y);
    }

    // Icons go onto the cropped output, upright, at their projected
    // (rotated + translated) screen position.
    if (layers && !layer_snapshot.empty()) {
        LayerRenderer::drawIconLayers(output, layer_snapshot, geom,
                                      heading, crop_x, crop_y, dpi_scale);
    }

    return output;
}

cv::Mat MapTileRenderer::drawMapByArea(double latitude, double longitude,
                                       double meters, int width, int height,
                                       double heading, int dpi,
                                       const LayerStore* layers) {
    if (meters <= 0.0) { LOGE("drawMapByArea: meters must be positive"); return cv::Mat(); }
    int min_dim = std::min(width, height);
    if (min_dim <= 0) { LOGE("drawMapByArea: invalid dimensions"); return cv::Mat(); }

    double meters_per_pixel = meters / static_cast<double>(min_dim);
    double cos_lat = std::cos(latitude * M_PI / 180.0);
    if (std::abs(cos_lat) < 1e-9) { LOGE("drawMapByArea: too close to pole"); return cv::Mat(); }

    double zoom = std::log2(156543.034 * cos_lat / meters_per_pixel);
    zoom = std::max(0.0, std::min(22.0, zoom));

    if (zoom < 10.0) {
        LOGW("drawMapByArea: zoom=%.2f - Web Mercator distortion may be "
             "significant at this scale", zoom);
    }
    /*LOGD("drawMapByArea: %.1fm over %dpx -> %.4f m/px -> zoom=%.3f",
         meters, min_dim, meters_per_pixel, zoom);
    */
    return drawMap(latitude, longitude, zoom, width, height, heading, dpi, layers);
}

MapTileRenderer::TileCoord MapTileRenderer::latLonToTile(double lat, double lon, int zoom) const {
    lat = std::max(-85.05112878, std::min(85.05112878, lat));
    double lat_rad = lat * M_PI / 180.0;
    int n = 1 << zoom;
    int x = static_cast<int>(std::floor((lon + 180.0) / 360.0 * n));
    int y = static_cast<int>(std::floor(
        (1.0 - std::asinh(std::tan(lat_rad)) / M_PI) / 2.0 * n));
    x = std::max(0, std::min(n - 1, x));
    y = std::max(0, std::min(n - 1, y));
    return {x, y, zoom};
}

void MapTileRenderer::latLonToPixel(double lat, double lon, int zoom,
                                    double& px, double& py) const {
    lat = std::max(-85.05112878, std::min(85.05112878, lat));
    double lat_rad = lat * M_PI / 180.0;
    double n = static_cast<double>(1 << zoom);
    px = (lon + 180.0) / 360.0 * n * TILE_SIZE;
    py = (1.0 - std::asinh(std::tan(lat_rad)) / M_PI) / 2.0 * n * TILE_SIZE;
}

MapTileRenderer::BoundingBox MapTileRenderer::calculateBounds(
    double lat, double lon, double zoom, int width, int height, int dpi) const {
    double mpp = 156543.034 * std::cos(lat * M_PI / 180.0) / std::pow(2.0, zoom);
    double w_m = width  * mpp * (dpi / 96.0);
    double h_m = height * mpp * (dpi / 96.0);
    double lat_off = (h_m / 2.0) / 111320.0;
    double lon_off = (w_m / 2.0) / (111320.0 * std::cos(lat * M_PI / 180.0));
    return { lat - lat_off, lat + lat_off, lon - lon_off, lon + lon_off };
}

std::vector<MapTileRenderer::TileCoord>
MapTileRenderer::getTilesInBounds(const BoundingBox& bounds, int zoom) const {
    TileCoord nw = latLonToTile(bounds.max_lat, bounds.min_lon, zoom);
    TileCoord se = latLonToTile(bounds.min_lat, bounds.max_lon, zoom);
    std::vector<TileCoord> tiles;
    for (int x = nw.x; x <= se.x; ++x)
        for (int y = nw.y; y <= se.y; ++y)
            tiles.push_back({x, y, zoom});
    return tiles;
}

cv::Mat MapTileRenderer::downloadTile(const TileCoord& coord) const {
    cv::Mat cached = getTileFromCache(coord);
    if (!cached.empty()) return cached;

    // Async path: queue a background fetch, return placeholder immediately.
    if (async_loader_) {
        async_loader_->request(buildTileUrl(coord),
                               AsyncTileLoader::TileKey{coord.x, coord.y, coord.z});
        return placeholder_tile_;
    }

    // Sync path (legacy / batch use).
    if (!fetcher_) return cv::Mat();

    std::string url = buildTileUrl(coord);
    auto bytes = fetcher_->fetch(url);
    if (bytes.empty()) return cv::Mat();

    cv::Mat tile = cv::imdecode(cv::Mat(bytes), cv::IMREAD_COLOR);
    if (tile.empty()) { LOGE("Failed to decode tile (z=%d x=%d y=%d)", coord.z, coord.x, coord.y); return cv::Mat(); }
    cv::cvtColor(tile, tile, cv::COLOR_BGR2RGBA);
    putTileInCache(coord, tile);
    return tile;
}

std::string MapTileRenderer::buildTileUrl(const TileCoord& coord) const {
    std::string url = tileset_url_;
    auto replace = [&](const std::string& token, const std::string& value) {
        size_t pos = url.find(token);
        if (pos != std::string::npos) url.replace(pos, token.size(), value);
    };
    replace("{z}", std::to_string(coord.z));
    replace("{y}", std::to_string(coord.y));
    replace("{x}", std::to_string(coord.x));
    return url;
}

cv::Mat MapTileRenderer::stitchAndCenter(const std::vector<TileCoord>& tiles,
                                         int tile_zoom, double scale_factor,
                                         double center_lat, double center_lon,
                                         int out_w, int out_h) const {
    if (tiles.empty()) return cv::Mat::zeros(out_h, out_w, CV_8UC4);

    int min_x = tiles[0].x, max_x = tiles[0].x;
    int min_y = tiles[0].y, max_y = tiles[0].y;
    for (const auto& t : tiles) {
        min_x = std::min(min_x, t.x); max_x = std::max(max_x, t.x);
        min_y = std::min(min_y, t.y); max_y = std::max(max_y, t.y);
    }

    int stitched_w = (max_x - min_x + 1) * TILE_SIZE;
    int stitched_h = (max_y - min_y + 1) * TILE_SIZE;
    cv::Mat stitched = cv::Mat::zeros(stitched_h, stitched_w, CV_8UC4);

    for (const auto& tc : tiles) {
        cv::Mat tile = downloadTile(tc);
        if (tile.empty()) continue;
        int dst_x = (tc.x - min_x) * TILE_SIZE;
        int dst_y = (tc.y - min_y) * TILE_SIZE;
        tile.copyTo(stitched(cv::Rect(dst_x, dst_y, TILE_SIZE, TILE_SIZE)));
    }

    double px, py;
    latLonToPixel(center_lat, center_lon, tile_zoom, px, py);
    double center_px_x = px - min_x * TILE_SIZE;
    double center_px_y = py - min_y * TILE_SIZE;

    cv::Mat scaled;
    if (std::abs(scale_factor - 1.0) > 1e-6) {
        int sw = std::max(1, static_cast<int>(std::round(stitched_w * scale_factor)));
        int sh = std::max(1, static_cast<int>(std::round(stitched_h * scale_factor)));
        int interp = (scale_factor < 1.0) ? cv::INTER_AREA : cv::INTER_LINEAR;
        cv::resize(stitched, scaled, cv::Size(sw, sh), 0, 0, interp);
    } else {
        scaled = stitched;
    }

    double out_cx = center_px_x * scale_factor;
    double out_cy = center_px_y * scale_factor;

    cv::Mat M = (cv::Mat_<double>(2, 3) <<
        1.0, 0.0, out_w * 0.5 - out_cx,
        0.0, 1.0, out_h * 0.5 - out_cy);
    cv::Mat out;
    cv::warpAffine(scaled, out, M, cv::Size(out_w, out_h),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0, 0));
    return out;
}

bool MapTileRenderer::initializeCache() {
    if (cache_dir_.empty()) return true;
    if (!create_directories(cache_dir_)) {
        LOGE("Failed to create cache directory: %s", cache_dir_.c_str());
        return false;
    }
    std::string db_path = cache_dir_ + "/tiles.db";
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        LOGE("Cannot open database: %s", sqlite3_errmsg(db_));
        return false;
    }
    const char* create_table = R"(
        CREATE TABLE IF NOT EXISTS tiles (
            x INTEGER, y INTEGER, z INTEGER,
            data BLOB, size INTEGER, last_accessed INTEGER,
            PRIMARY KEY (x, y, z));
        CREATE INDEX IF NOT EXISTS idx_last_accessed ON tiles(last_accessed);
    )";
    if (sqlite3_exec(db_, create_table, nullptr, nullptr, nullptr) != SQLITE_OK) {
        LOGE("Cannot create table: %s", sqlite3_errmsg(db_));
        sqlite3_close(db_); db_ = nullptr;
        return false;
    }
    return true;
}

void MapTileRenderer::cleanupCache() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

cv::Mat MapTileRenderer::getTileFromCache(const TileCoord& coord) const {
    TileKey key = coordToKey(coord);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = memory_cache_.find(key);
        if (it != memory_cache_.end()) {
            lru_list_.erase(lru_map_[key]);
            lru_list_.push_front(key);
            lru_map_[key] = lru_list_.begin();
            return it->second.clone();
        }
    }
    if (!db_) return cv::Mat();
    cv::Mat tile = loadTileFromDisk(coord);
    if (tile.empty()) return cv::Mat();
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        size_t tile_size = estimateTileSize(tile);
        while (current_memory_usage_ + tile_size > max_memory_cache_bytes_
               && !lru_list_.empty()) evictMemoryCache();
        memory_cache_[key] = tile.clone();
        lru_list_.push_front(key);
        lru_map_[key] = lru_list_.begin();
        current_memory_usage_ += tile_size;
    }
    return tile;
}

void MapTileRenderer::putTileInCache(const TileCoord& coord, const cv::Mat& tile) const {
    if (tile.empty()) return;
    TileKey key = coordToKey(coord);
    size_t tile_size = estimateTileSize(tile);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = memory_cache_.find(key);
        if (it != memory_cache_.end()) {
            lru_list_.erase(lru_map_[key]);
            current_memory_usage_ -= estimateTileSize(it->second);
        }
        while (current_memory_usage_ + tile_size > max_memory_cache_bytes_
               && !lru_list_.empty()) evictMemoryCache();
        memory_cache_[key] = tile.clone();
        lru_list_.push_front(key);
        lru_map_[key] = lru_list_.begin();
        current_memory_usage_ += tile_size;
    }
    if (db_) saveTileToDisk(coord, tile);
}

cv::Mat MapTileRenderer::loadTileFromDisk(const TileCoord& coord) const {
    if (!db_) return cv::Mat();
    const char* sql = "SELECT data FROM tiles WHERE x=? AND y=? AND z=?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return cv::Mat();
    sqlite3_bind_int(stmt, 1, coord.x);
    sqlite3_bind_int(stmt, 2, coord.y);
    sqlite3_bind_int(stmt, 3, coord.z);

    cv::Mat tile;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blob_size = sqlite3_column_bytes(stmt, 0);
        if (blob && blob_size > 0) {
            std::vector<uchar> buffer(static_cast<const uchar*>(blob),
                                      static_cast<const uchar*>(blob) + blob_size);
            tile = cv::imdecode(buffer, cv::IMREAD_COLOR);
            if (!tile.empty()) cv::cvtColor(tile, tile, cv::COLOR_BGR2RGBA);
        }
        const char* upd = "UPDATE tiles SET last_accessed=? WHERE x=? AND y=? AND z=?";
        sqlite3_stmt* u;
        if (sqlite3_prepare_v2(db_, upd, -1, &u, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(u, 1, std::time(nullptr));
            sqlite3_bind_int(u, 2, coord.x);
            sqlite3_bind_int(u, 3, coord.y);
            sqlite3_bind_int(u, 4, coord.z);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
    }
    sqlite3_finalize(stmt);
    return tile;
}

void MapTileRenderer::saveTileToDisk(const TileCoord& coord, const cv::Mat& tile) const {
    if (!db_ || tile.empty()) return;
    cv::Mat bgr;
    cv::cvtColor(tile, bgr, cv::COLOR_RGBA2BGR);
    std::vector<uchar> buffer;
    if (!cv::imencode(".png", bgr, buffer)) return;

    const char* sql =
        "INSERT OR REPLACE INTO tiles (x, y, z, data, size, last_accessed) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, coord.x);
    sqlite3_bind_int(stmt, 2, coord.y);
    sqlite3_bind_int(stmt, 3, coord.z);
    sqlite3_bind_blob(stmt, 4, buffer.data(), buffer.size(), SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, static_cast<int>(buffer.size()));
    sqlite3_bind_int64(stmt, 6, std::time(nullptr));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    evictDiskCache();
}

void MapTileRenderer::evictMemoryCache() const {
    if (lru_list_.empty()) return;
    TileKey oldest = lru_list_.back();
    lru_list_.pop_back();
    auto it = memory_cache_.find(oldest);
    if (it != memory_cache_.end()) {
        current_memory_usage_ -= estimateTileSize(it->second);
        memory_cache_.erase(it);
    }
    lru_map_.erase(oldest);
}

void MapTileRenderer::evictDiskCache() const {
    if (!db_) return;
    const char* size_sql = "SELECT SUM(size) FROM tiles";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, size_sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t total = sqlite3_column_int64(stmt, 0);
        if (total > static_cast<int64_t>(max_disk_cache_bytes_)) {
            const char* del =
                "DELETE FROM tiles WHERE rowid IN "
                "(SELECT rowid FROM tiles ORDER BY last_accessed LIMIT ?)";
            sqlite3_stmt* d;
            int to_delete = static_cast<int>(total - max_disk_cache_bytes_) / 10000 + 1;
            if (sqlite3_prepare_v2(db_, del, -1, &d, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(d, 1, to_delete);
                sqlite3_step(d);
                sqlite3_finalize(d);
            }
        }
    }
    sqlite3_finalize(stmt);
}

size_t MapTileRenderer::estimateTileSize(const cv::Mat& tile) const {
    return tile.total() * tile.elemSize();
}

MapTileRenderer::TileKey MapTileRenderer::coordToKey(const TileCoord& coord) const {
    return {coord.x, coord.y, coord.z};
}
