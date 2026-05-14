#pragma once

#include "MapLayer.h"

#include <opencv2/core.hpp>
#include <vector>

// Internal helper. Not part of the public API — included only by
// MapTileRenderer.cpp.
//
// The renderer's coordinate pipeline produces an oversized pre-rotation
// canvas that is then rotated and cropped. Geographic layers (rectangles,
// circles) are drawn onto that canvas before rotation, so they rotate with
// the map. Icons are drawn after rotation + crop, in final output space,
// upright relative to the viewer.

namespace LayerRenderer {

// Coordinate-transform info the renderer hands to the layer code so it can
// place layers in the right pixel coordinates without re-deriving the math.
struct CanvasGeometry {
    // Center of the canvas in lat/lon and its pixel coords within the
    // pre-rotation canvas.
    double center_lat;
    double center_lon;
    double center_px;   // x in canvas pixels
    double center_py;   // y in canvas pixels

    // Output meters per pixel at the canvas's effective zoom and at
    // center_lat. Same in x and y in the local-tangent-plane approximation.
    double meters_per_pixel;

    // Canvas dimensions in pixels (pre-rotation, pre-crop).
    int canvas_w;
    int canvas_h;
};

// Draw all geographic layers (GeoRect, GeoCircle, fence boundaries) onto
// `canvas` in pre-rotation coordinates. Alpha-blends so translucent fills
// work.
//
// Note: this draws the fence *boundary outline* but not the hatches. Hatches
// are screen-aligned (always 45°) and are drawn post-rotation by
// drawFenceHatches.
void drawGeographicLayers(cv::Mat& canvas,
                          const std::vector<std::shared_ptr<MapLayer>>& layers,
                          const CanvasGeometry& geom);

// Draw the fence's hatch pattern on `output` (post-rotation, post-crop).
// The hatch direction is fixed in screen space at 45°. The fence's inside
// mask is rebuilt in canvas space, rotated/cropped the same way the canvas
// was, and used to clear out the allowed area.
//
// No-op if no FenceLayer is present in `layers`.
void drawFenceHatches(cv::Mat& output,
                      const std::vector<std::shared_ptr<MapLayer>>& layers,
                      const CanvasGeometry& geom,
                      double rotation_deg,
                      int crop_x, int crop_y);

// Draw all icon layers onto `output` in post-rotation output-space
// coordinates. Needs the same canvas geometry as above plus the rotation
// angle (degrees, CW positive) and the crop offset that was applied to get
// from canvas space to output space.
void drawIconLayers(cv::Mat& output,
                    const std::vector<std::shared_ptr<MapLayer>>& layers,
                    const CanvasGeometry& geom,
                    double rotation_deg,
                    int crop_x,
                    int crop_y,
                    double dpi_scale);

}  // namespace LayerRenderer
