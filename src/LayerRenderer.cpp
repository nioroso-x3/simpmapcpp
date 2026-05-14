#include "LayerRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <opencv2/imgproc.hpp>

#if defined(__ANDROID__)
  #include <android/log.h>
#endif

namespace {

// Convert lat/lon -> canvas pixel using local-tangent-plane (linear)
// approximation. Accurate enough at zoom >= 10 where the canvas spans
// well under a degree of latitude. The renderer already has full Web
// Mercator math but doing it here per-layer would be wasteful, and the
// linear approximation is what users get when they use drawMapByArea
// anyway (which assumes minimal distortion).
void latLonToCanvasPx(double lat, double lon,
                      const LayerRenderer::CanvasGeometry& g,
                      double& out_x, double& out_y) {
    constexpr double METERS_PER_DEG_LAT = 111320.0;
    double cos_center = std::cos(g.center_lat * M_PI / 180.0);
    double meters_per_deg_lon = METERS_PER_DEG_LAT * cos_center;

    double dx_m = (lon - g.center_lon) * meters_per_deg_lon;
    // y axis is flipped: north is "up" in lat but smaller y in image space
    double dy_m = -(lat - g.center_lat) * METERS_PER_DEG_LAT;

    out_x = g.center_px + dx_m / g.meters_per_pixel;
    out_y = g.center_py + dy_m / g.meters_per_pixel;
}

// Alpha-composite `overlay` onto `dest`. Both must be CV_8UC4 (RGBA).
// `overlay` alpha drives the blend; opaque overlay pixels fully replace
// dest, transparent ones leave dest unchanged, partial ones blend.
void alphaCompositeOver(cv::Mat& dest, const cv::Mat& overlay) {
    CV_Assert(dest.size() == overlay.size());
    CV_Assert(dest.type() == CV_8UC4 && overlay.type() == CV_8UC4);

    for (int y = 0; y < dest.rows; ++y) {
        cv::Vec4b* drow = dest.ptr<cv::Vec4b>(y);
        const cv::Vec4b* orow = overlay.ptr<cv::Vec4b>(y);
        for (int x = 0; x < dest.cols; ++x) {
            const cv::Vec4b& o = orow[x];
            if (o[3] == 0) continue;            // fully transparent
            if (o[3] == 255) { drow[x] = o; continue; }  // fully opaque
            float a = o[3] / 255.0f;
            float ia = 1.0f - a;
            cv::Vec4b& d = drow[x];
            d[0] = static_cast<uint8_t>(o[0] * a + d[0] * ia);
            d[1] = static_cast<uint8_t>(o[1] * a + d[1] * ia);
            d[2] = static_cast<uint8_t>(o[2] * a + d[2] * ia);
            d[3] = std::max(d[3], o[3]);
        }
    }
}

// ---------------------------------------------------------------------------
// Geographic shape drawing (all draw into an overlay that gets composited)
// ---------------------------------------------------------------------------

void drawRect(cv::Mat& overlay, const GeoRectLayer& r,
              const LayerRenderer::CanvasGeometry& g) {
    double cx, cy;
    latLonToCanvasPx(r.latitude, r.longitude, g, cx, cy);

    // Width/height in pixels
    double half_w_px = (r.width_m  * 0.5) / g.meters_per_pixel;
    double half_h_px = (r.height_m * 0.5) / g.meters_per_pixel;

#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_DEBUG, "LayerRenderer",
        "Rect at canvas (%.1f, %.1f), half_w=%.2fpx half_h=%.2fpx",
        cx, cy, half_w_px, half_h_px);
#else
    std::fprintf(stderr, "[DEBUG] Rect at canvas (%.1f, %.1f), "
                 "half_w=%.2fpx half_h=%.2fpx\n",
                 cx, cy, half_w_px, half_h_px);
#endif

    // Four corners in local rect space, then rotated by heading, then
    // translated to (cx, cy).
    double a = r.heading_deg * M_PI / 180.0;
    double cosA = std::cos(a), sinA = std::sin(a);

    std::array<cv::Point, 4> corners;
    const double dx[4] = {-half_w_px, +half_w_px, +half_w_px, -half_w_px};
    const double dy[4] = {-half_h_px, -half_h_px, +half_h_px, +half_h_px};
    for (int i = 0; i < 4; ++i) {
        double rx = dx[i] * cosA - dy[i] * sinA;
        double ry = dx[i] * sinA + dy[i] * cosA;
        corners[i] = cv::Point(static_cast<int>(std::round(cx + rx)),
                               static_cast<int>(std::round(cy + ry)));
    }

    if (r.fill_color[3] > 0) {
        cv::fillConvexPoly(overlay, corners.data(), 4, r.fill_color, cv::LINE_AA);
    }
    if (r.stroke_color[3] > 0 && r.stroke_width_px > 0) {
        const cv::Point* pts = corners.data();
        int n = 4;
        cv::polylines(overlay, &pts, &n, 1, true, r.stroke_color,
                      r.stroke_width_px, cv::LINE_AA);
    }
}

void drawCircle(cv::Mat& overlay, const GeoCircleLayer& c,
                const LayerRenderer::CanvasGeometry& g) {
    double cx, cy;
    latLonToCanvasPx(c.latitude, c.longitude, g, cx, cy);
    double radius_px = c.radius_m / g.meters_per_pixel;

    cv::Point center(static_cast<int>(std::round(cx)),
                     static_cast<int>(std::round(cy)));
    int r_px = std::max(1, static_cast<int>(std::round(radius_px)));

    if (c.fill_color[3] > 0) {
        cv::circle(overlay, center, r_px, c.fill_color, cv::FILLED, cv::LINE_AA);
    }
    if (c.stroke_color[3] > 0 && c.stroke_width_px > 0) {
        cv::circle(overlay, center, r_px, c.stroke_color,
                   c.stroke_width_px, cv::LINE_AA);
    }
}

// ---------------------------------------------------------------------------
// Fence rendering (diagonal hatches in the area OUTSIDE the fence boundary)
// ---------------------------------------------------------------------------

// Build a binary mask (CV_8UC1) where 1 = inside the fence, 0 = outside.
void buildFenceMask(cv::Mat& mask, const FenceLayer& fence,
                    const LayerRenderer::CanvasGeometry& g) {
    mask = cv::Mat::zeros(g.canvas_h, g.canvas_w, CV_8UC1);

    if (fence.shape == FenceLayer::Shape::Circle) {
        double cx, cy;
        latLonToCanvasPx(fence.latitude, fence.longitude, g, cx, cy);
        int r_px = std::max(1, static_cast<int>(
            std::round(fence.radius_m / g.meters_per_pixel)));
        cv::circle(mask,
                   cv::Point(static_cast<int>(std::round(cx)),
                             static_cast<int>(std::round(cy))),
                   r_px, cv::Scalar(1), cv::FILLED, cv::LINE_AA);
    } else {
        if (fence.polygon.size() < 3) return;
        std::vector<cv::Point> pts;
        pts.reserve(fence.polygon.size());
        for (const auto& [lat, lon] : fence.polygon) {
            double x, y;
            latLonToCanvasPx(lat, lon, g, x, y);
            pts.emplace_back(static_cast<int>(std::round(x)),
                             static_cast<int>(std::round(y)));
        }
        const cv::Point* ppts = pts.data();
        int npts = static_cast<int>(pts.size());
        cv::fillPoly(mask, &ppts, &npts, 1, cv::Scalar(1), cv::LINE_AA);
    }
}

void drawFenceBoundary(cv::Mat& overlay, const FenceLayer& fence,
                       const LayerRenderer::CanvasGeometry& g) {
    if (fence.boundary_color[3] == 0 || fence.boundary_width_px <= 0) return;

    if (fence.shape == FenceLayer::Shape::Circle) {
        double cx, cy;
        latLonToCanvasPx(fence.latitude, fence.longitude, g, cx, cy);
        int r_px = std::max(1, static_cast<int>(
            std::round(fence.radius_m / g.meters_per_pixel)));
        cv::circle(overlay,
                   cv::Point(static_cast<int>(std::round(cx)),
                             static_cast<int>(std::round(cy))),
                   r_px, fence.boundary_color,
                   fence.boundary_width_px, cv::LINE_AA);
    } else if (fence.polygon.size() >= 3) {
        std::vector<cv::Point> pts;
        pts.reserve(fence.polygon.size());
        for (const auto& [lat, lon] : fence.polygon) {
            double x, y;
            latLonToCanvasPx(lat, lon, g, x, y);
            pts.emplace_back(static_cast<int>(std::round(x)),
                             static_cast<int>(std::round(y)));
        }
        const cv::Point* ppts = pts.data();
        int npts = static_cast<int>(pts.size());
        cv::polylines(overlay, &ppts, &npts, 1, true,
                      fence.boundary_color,
                      fence.boundary_width_px, cv::LINE_AA);
    }
}

// ---------------------------------------------------------------------------
// Icon drawing
// ---------------------------------------------------------------------------

void drawBuiltinSymbol(cv::Mat& dest, cv::Point center, int size_px,
                       IconSymbol symbol, cv::Scalar color, int stroke) {
    int half = std::max(1, size_px / 2);
    switch (symbol) {
        case IconSymbol::Square: {
            cv::Rect r(center.x - half, center.y - half, size_px, size_px);
            cv::rectangle(dest, r, color, cv::FILLED, cv::LINE_AA);
            break;
        }
        case IconSymbol::Circle: {
            cv::circle(dest, center, half, color, cv::FILLED, cv::LINE_AA);
            break;
        }
        case IconSymbol::Cross: {
            int s = std::max(1, stroke);
            cv::line(dest, {center.x - half, center.y - half},
                           {center.x + half, center.y + half},
                     color, s, cv::LINE_AA);
            cv::line(dest, {center.x - half, center.y + half},
                           {center.x + half, center.y - half},
                     color, s, cv::LINE_AA);
            break;
        }
        case IconSymbol::Triangle: {
            cv::Point pts[3] = {
                {center.x,        center.y - half},
                {center.x - half, center.y + half},
                {center.x + half, center.y + half},
            };
            cv::fillConvexPoly(dest, pts, 3, color, cv::LINE_AA);
            break;
        }
        case IconSymbol::Diamond: {
            cv::Point pts[4] = {
                {center.x,        center.y - half},
                {center.x + half, center.y       },
                {center.x,        center.y + half},
                {center.x - half, center.y       },
            };
            cv::fillConvexPoly(dest, pts, 4, color, cv::LINE_AA);
            break;
        }
    }
}

void drawIcon(cv::Mat& output, const IconLayer& icon,
              const LayerRenderer::CanvasGeometry& g,
              double rotation_deg, int crop_x, int crop_y,
              double dpi_scale) {
    // Step 1: lat/lon -> pre-rotation canvas pixel
    double px, py;
    latLonToCanvasPx(icon.latitude, icon.longitude, g, px, py);

    // Step 2: apply rotation about canvas center
    double a = -rotation_deg * M_PI / 180.0;  // canvas rotation is CW; reverse sign for the same rotation matrix the renderer used
    double cx = g.canvas_w * 0.5;
    double cy = g.canvas_h * 0.5;
    double rx = (px - cx) * std::cos(a) - (py - cy) * std::sin(a) + cx;
    double ry = (px - cx) * std::sin(a) + (py - cy) * std::cos(a) + cy;

    // Step 3: apply crop offset to land in output coordinates
    int ox = static_cast<int>(std::round(rx - crop_x));
    int oy = static_cast<int>(std::round(ry - crop_y));

    // Bail if the icon center is off-screen by more than its half-size
    int size_px = std::max(1, static_cast<int>(std::round(icon.size_px * dpi_scale)));
    int half = size_px / 2;
    if (ox + half < 0 || oy + half < 0 ||
        ox - half >= output.cols || oy - half >= output.rows) {
        return;
    }

    cv::Point center(ox, oy);

    if (!icon.pixel_data.empty()) {
        // PNG-style icon: scale to size_px, alpha-composite at center.
        cv::Mat scaled;
        if (icon.pixel_data.cols != size_px || icon.pixel_data.rows != size_px) {
            cv::resize(icon.pixel_data, scaled, cv::Size(size_px, size_px),
                       0, 0, cv::INTER_AREA);
        } else {
            scaled = icon.pixel_data;
        }
        // Source must be RGBA. If user passed RGB, convert.
        cv::Mat rgba;
        if (scaled.channels() == 3) {
            cv::cvtColor(scaled, rgba, cv::COLOR_RGB2RGBA);
        } else {
            rgba = scaled;
        }

        // Compute clipped destination region
        int x0 = ox - half, y0 = oy - half;
        cv::Rect dest_rect(x0, y0, size_px, size_px);
        cv::Rect canvas_rect(0, 0, output.cols, output.rows);
        cv::Rect clipped = dest_rect & canvas_rect;
        if (clipped.area() == 0) return;

        cv::Rect src_rect(clipped.x - x0, clipped.y - y0,
                          clipped.width, clipped.height);

        cv::Mat dst_view = output(clipped);
        cv::Mat src_view = rgba(src_rect);
        alphaCompositeOver(dst_view, src_view);
    } else {
        int stroke = std::max(1, static_cast<int>(std::round(icon.stroke_width_px * dpi_scale)));
        drawBuiltinSymbol(output, center, size_px, icon.symbol, icon.color, stroke);
    }
}

}  // namespace

namespace LayerRenderer {

void drawGeographicLayers(cv::Mat& canvas,
                          const std::vector<std::shared_ptr<MapLayer>>& layers,
                          const CanvasGeometry& geom) {
    if (layers.empty()) return;

    // Draw onto a separate transparent overlay, then alpha-blend over the
    // canvas. This is what gives us proper translucent fills.
    cv::Mat overlay(canvas.size(), CV_8UC4, cv::Scalar(0, 0, 0, 0));
    bool drew_anything = false;

    // Pass 1: non-fence geographic shapes (rectangles, circles).
    for (const auto& l : layers) {
        if (!l || !l->visible || !l->isGeographic()) continue;
        if (auto* r = dynamic_cast<GeoRectLayer*>(l.get())) {
            drawRect(overlay, *r, geom);
            drew_anything = true;
        } else if (auto* c = dynamic_cast<GeoCircleLayer*>(l.get())) {
            drawCircle(overlay, *c, geom);
            drew_anything = true;
        }
    }

    // Pass 2: at most one fence boundary, drawn last so it sits on top of
    // other geographic shapes. The hatches are drawn post-rotation by
    // drawFenceHatches so they stay screen-aligned at 45°.
    for (const auto& l : layers) {
        if (!l || !l->visible) continue;
        if (auto* f = dynamic_cast<FenceLayer*>(l.get())) {
            drawFenceBoundary(overlay, *f, geom);
            drew_anything = true;
            break;  // single-fence semantics
        }
    }

    if (drew_anything) alphaCompositeOver(canvas, overlay);
}

void drawIconLayers(cv::Mat& output,
                    const std::vector<std::shared_ptr<MapLayer>>& layers,
                    const CanvasGeometry& geom,
                    double rotation_deg, int crop_x, int crop_y,
                    double dpi_scale) {
    for (const auto& l : layers) {
        if (!l || !l->visible) continue;
        auto* icon = dynamic_cast<IconLayer*>(l.get());
        if (!icon) continue;
        drawIcon(output, *icon, geom, rotation_deg, crop_x, crop_y, dpi_scale);
    }
}

void drawFenceHatches(cv::Mat& output,
                      const std::vector<std::shared_ptr<MapLayer>>& layers,
                      const CanvasGeometry& geom,
                      double rotation_deg, int crop_x, int crop_y) {
    // Find the first visible fence.
    FenceLayer* fence = nullptr;
    for (const auto& l : layers) {
        if (!l || !l->visible) continue;
        if (auto* f = dynamic_cast<FenceLayer*>(l.get())) { fence = f; break; }
    }
    if (!fence) return;

    // Build the inside mask in pre-rotation canvas space.
    cv::Mat canvas_mask;
    buildFenceMask(canvas_mask, *fence, geom);

    // Rotate the mask the same way the canvas was rotated, then crop.
    cv::Mat rotated_mask;
    if (std::abs(rotation_deg) > 0.01) {
        cv::Point2f c(geom.canvas_w * 0.5f, geom.canvas_h * 0.5f);
        cv::Mat M = cv::getRotationMatrix2D(c, rotation_deg, 1.0);
        cv::warpAffine(canvas_mask, rotated_mask, M,
                       cv::Size(geom.canvas_w, geom.canvas_h),
                       cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
    } else {
        rotated_mask = canvas_mask;
    }

    // Crop to output size.
    int crop_w = std::min(output.cols, rotated_mask.cols - crop_x);
    int crop_h = std::min(output.rows, rotated_mask.rows - crop_y);
    cv::Mat inside_mask = rotated_mask(cv::Rect(crop_x, crop_y, crop_w, crop_h));

    // Inside mask values are 0 or 1; we want 0 (inside) or 255 (outside) for
    // the alpha multiply below.
    cv::Mat outside_mask;
    cv::bitwise_not(inside_mask * 255, outside_mask);

    // Render screen-aligned 45° hatches into a full-output RGBA overlay.
    cv::Mat hatch(output.rows, output.cols, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    int spacing   = std::max(2, fence->hatch_spacing_px);
    int thickness = std::max(1, fence->hatch_thickness_px);

    // \\\ direction: lines y = x + c, c ranges from -w to h with step spacing
    int w = output.cols, h = output.rows;
    for (int c = -w; c <= h; c += spacing) {
        int x1 = std::max(0, -c);
        int y1 = x1 + c;
        int x2 = std::min(w - 1, h - 1 - c);
        int y2 = x2 + c;
        if (x1 > x2 || y1 < 0 || y2 >= h) continue;
        cv::line(hatch, cv::Point(x1, y1), cv::Point(x2, y2),
                 fence->hatch_color, thickness, cv::LINE_AA);
    }

    // Zero alpha inside the fence so only the outside region remains hatched.
    std::vector<cv::Mat> ch(4);
    cv::split(hatch, ch);
    cv::bitwise_and(ch[3], outside_mask, ch[3]);
    cv::merge(ch, hatch);

    alphaCompositeOver(output, hatch);
}

}  // namespace LayerRenderer
