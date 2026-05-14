#pragma once

#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Layer types
// ---------------------------------------------------------------------------

enum class IconSymbol {
    Square,
    Circle,
    Cross,       // X shape
    Triangle,
    Diamond,
};

// Polymorphic base. All layers have a geographic anchor (latitude/longitude)
// and an enabled flag. The id is for user-side tracking — useful when you
// want to look up or remove a specific layer later. The renderer doesn't
// care about it.
class MapLayer {
public:
    virtual ~MapLayer() = default;

    double latitude  = 0.0;
    double longitude = 0.0;
    bool   visible   = true;
    std::string id;

    // True if this layer represents a real-world geographic feature whose
    // size scales with zoom and that should rotate with the map.
    // False for screen-space layers like icons whose pixel size is fixed
    // and which stay upright relative to the viewer.
    virtual bool isGeographic() const = 0;
};

// Pixel-sized marker that stays the same size regardless of zoom and stays
// upright when the map rotates. Anchor is the icon's center.
class IconLayer : public MapLayer {
public:
    // If pixel_data is non-empty, it is used directly (expected to be RGBA,
    // CV_8UC4). Otherwise the built-in `symbol` is drawn in `color`.
    cv::Mat     pixel_data;
    IconSymbol  symbol = IconSymbol::Circle;
    cv::Scalar  color{255, 0, 0, 255};   // RGBA
    int         size_px = 16;             // rendered size in screen pixels at 96 DPI
    int         stroke_width_px = 2;      // for built-in symbols only

    bool isGeographic() const override { return false; }
};

// Axis-aligned-when-heading=0 rectangle defined in meters. heading_deg
// rotates the rectangle clockwise from north. The center is at (lat, lon).
class GeoRectLayer : public MapLayer {
public:
    double      width_m       = 10.0;
    double      height_m      = 10.0;
    double      heading_deg   = 0.0;
    cv::Scalar  fill_color    {0, 0, 255, 80};    // RGBA, alpha < 255 for translucent
    cv::Scalar  stroke_color  {0, 0, 255, 255};
    int         stroke_width_px = 2;

    bool isGeographic() const override { return true; }
};

// Geographic circle defined by radius in meters, centered at (lat, lon).
class GeoCircleLayer : public MapLayer {
public:
    double      radius_m      = 10.0;
    cv::Scalar  fill_color    {0, 255, 0, 80};
    cv::Scalar  stroke_color  {0, 255, 0, 255};
    int         stroke_width_px = 2;
    int         segments      = 64;   // number of polygon points for fill

    bool isGeographic() const override { return true; }
};

// Geofence: defines an allowed (inside) area; everything outside is rendered
// with a diagonal hatch pattern to mark the forbidden zone.
//
// Only one fence is active at a time. If the LayerStore contains multiple
// FenceLayers, only the first visible one is applied — adding a second one
// is not an error but it's a no-op for rendering.
//
// Two shapes:
//   Circle  — uses the layer's (latitude, longitude) as the center, plus
//             radius_m. Set via setCircle() or by populating fields directly.
//   Polygon — uses polygon as a list of (lat, lon) points in order. The
//             polygon is auto-closed (you don't need to repeat the first
//             point). The layer's (latitude, longitude) is unused.
class FenceLayer : public MapLayer {
public:
    enum class Shape { Circle, Polygon };
    Shape  shape    = Shape::Circle;
    double radius_m = 100.0;
    std::vector<std::pair<double, double>> polygon;

    // Visualization
    cv::Scalar hatch_color        {180, 0, 180, 180};  // purple, RGBA
    int        hatch_spacing_px   = 10;
    int        hatch_thickness_px = 1;
    cv::Scalar boundary_color     {180, 0, 180, 255};
    int        boundary_width_px  = 2;

    // Convenience setters
    void setCircle(double center_lat, double center_lon, double radius_meters) {
        latitude = center_lat;
        longitude = center_lon;
        radius_m = radius_meters;
        shape = Shape::Circle;
    }

    void setPolygon(std::vector<std::pair<double, double>> points) {
        polygon = std::move(points);
        shape = Shape::Polygon;
    }

    bool isGeographic() const override { return true; }
};


// ---------------------------------------------------------------------------
// LayerStore
// ---------------------------------------------------------------------------

// Thread-safe container that owns layers. Pass to drawMap/drawMapByArea
// to have layers composited onto the rendered map.
//
// The renderer reads the layers list under the same mutex used by add/remove,
// so it is safe to mutate from another thread while a render is in flight
// (the render will see a consistent snapshot).
class LayerStore {
public:
    // Adds a layer. If layer->id is empty, an id of the form "layer-N" is
    // assigned. Returns the id (assigned or pre-existing).
    std::string add(std::shared_ptr<MapLayer> layer);

    // Removes a layer by id. Returns true if a layer was removed.
    bool remove(const std::string& id);

    // Removes all layers.
    void clear();

    // Returns a snapshot of the current layers. Safe to call while another
    // thread mutates the store.
    std::vector<std::shared_ptr<MapLayer>> snapshot() const;

    // Look up a layer by id. Returns nullptr if not found.
    std::shared_ptr<MapLayer> get(const std::string& id) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<MapLayer>> layers_;
    int next_auto_id_ = 0;
};
