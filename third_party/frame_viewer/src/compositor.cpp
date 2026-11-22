#include "compositor.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace fv::detail {

// Forward declarations of helper functions
static bool resize_supports(int depth);
static const cv::Mat& level_image(FrameEntry& e, int level);
static int clamp_int(double v, int lo, int hi);
static bool draw_frame(cv::Mat& canvas,
                       FrameEntry& e,
                       const Camera& cam,
                       ClockType::time_point now);
static bool draw_rotated_frame(cv::Mat& canvas,
                               FrameEntry& e,
                               const Camera& cam,
                               ClockType::time_point now);

cv::Mat to_bgr8(const cv::Mat& in) {
    cv::Mat src = in;
    if (in.depth() != CV_8U) {
        double scale = 1.0;
        switch (in.depth()) {
            case CV_16U:
                scale = 1.0 / 257.0;
                break;
            case CV_16S:
                scale = 1.0 / 128.0;
                break;
            case CV_32F:
            case CV_64F:
                scale = 255.0;
                break;
            default:
                break;
        }
        in.convertTo(src, CV_8U, scale);
    }

    cv::Mat out;
    switch (src.channels()) {
        case 3:
            return src;  // already BGR (shallow when no depth conversion happened)
        case 1:
            cv::cvtColor(src, out, cv::COLOR_GRAY2BGR);
            break;
        case 4:
            cv::cvtColor(src, out, cv::COLOR_BGRA2BGR);
            break;
        default: {
            cv::Mat first;
            cv::extractChannel(src, first, 0);
            cv::cvtColor(first, out, cv::COLOR_GRAY2BGR);
            break;
        }
    }
    return out;
}

bool compose(cv::Mat& canvas,
             std::vector<FrameEntry>& frames,
             const Camera& cam,
             ClockType::time_point now) {
    canvas.setTo(cv::Scalar(32, 32, 32));

    std::vector<FrameEntry*> ordered;
    ordered.reserve(frames.size());
    for (auto& e : frames)
        if (!e.image.empty() && e.world_rect.width > 0 && e.world_rect.height > 0)
            ordered.push_back(&e);

    std::sort(ordered.begin(), ordered.end(), [](const FrameEntry* a, const FrameEntry* b) {
        return a->layer != b->layer ? a->layer < b->layer : a->order < b->order;
    });

    bool used_fast = false;
    for (FrameEntry* e : ordered)
        used_fast |= draw_frame(canvas, *e, cam, now);
    return used_fast;
}

cv::Rect2d content_bounds(const std::vector<FrameEntry>& frames) {
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const auto& e : frames) {
        if (e.image.empty() || e.world_rect.width <= 0 || e.world_rect.height <= 0) continue;
        const cv::Rect2d r = e.bounds();
        if (!any) {
            x0 = r.x;
            y0 = r.y;
            x1 = r.x + r.width;
            y1 = r.y + r.height;
            any = true;
        } else {
            x0 = std::min(x0, r.x);
            y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.x + r.width);
            y1 = std::max(y1, r.y + r.height);
        }
    }
    return any ? cv::Rect2d(x0, y0, x1 - x0, y1 - y0) : cv::Rect2d();
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
static bool resize_supports(int depth) {
    return depth == CV_8U || depth == CV_16U || depth == CV_32F || depth == CV_64F;
}

/** Return the frame's image downscaled by 2^level, building and caching it on first use. */
static const cv::Mat& level_image(FrameEntry& e, int level) {
    if (level <= 0) return e.image;
    auto& levels = e.cache->levels;
    if (auto it = levels.find(level); it != levels.end()) return it->second;

    const int div = 1 << level;
    const cv::Size size(std::max(1, (e.image.cols + div - 1) / div),
                        std::max(1, (e.image.rows + div - 1) / div));
    cv::Mat source = resize_supports(e.image.depth()) ? e.image : to_bgr8(e.image);
    cv::Mat out;
    cv::resize(source, out, size, 0, 0, cv::INTER_AREA);
    return levels.emplace(level, std::move(out)).first->second;
}

static int clamp_int(double v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return static_cast<int>(v);
}

/** Draw one frame onto the canvas. Return true if the fast (live) shrinking method was used. */
static bool draw_frame(cv::Mat& canvas,
                       FrameEntry& e,
                       const Camera& cam,
                       ClockType::time_point now) {
    if (e.angle_deg != 0.0) return draw_rotated_frame(canvas, e, cam, now);

    const int W = canvas.cols, H = canvas.rows;
    const cv::Rect2d& wr = e.world_rect;

    // Frame edges in (continuous) screen coordinates.
    const double sx0 = (wr.x - cam.offset.x) * cam.zoom;
    const double sx1 = (wr.x + wr.width - cam.offset.x) * cam.zoom;
    const double sy0 = (wr.y - cam.offset.y) * cam.zoom;
    const double sy1 = (wr.y + wr.height - cam.offset.y) * cam.zoom;
    if (sx1 - sx0 <= 0.0 || sy1 - sy0 <= 0.0) return false;

    // Destination pixels, rounded the same way for every frame so neighbours meet without gaps.
    const int dx0 = clamp_int(std::round(sx0), 0, W), dx1 = clamp_int(std::round(sx1), 0, W);
    const int dy0 = clamp_int(std::round(sy0), 0, H), dy1 = clamp_int(std::round(sy1), 0, H);
    if (dx1 <= dx0 || dy1 <= dy0) return false;  // off-screen

    const cv::Rect dst_rect(dx0, dy0, dx1 - dx0, dy1 - dy0);
    cv::Mat dst = canvas(dst_rect);

    // Live frame being shrunk: sample the visible region straight to the destination size.
    // ~10-20x cheaper than area averaging a large frame, and nothing is cached for a frame
    // that's about to be replaced.
    const double full_kx = e.image.cols / (sx1 - sx0), full_ky = e.image.rows / (sy1 - sy0);
    if (full_kx >= 1.0 && full_ky >= 1.0 && e.is_live(now)) {
        const int rx0 = clamp_int(std::floor((dx0 - sx0) * full_kx), 0, e.image.cols - 1);
        const int rx1 = clamp_int(std::ceil((dx1 - sx0) * full_kx), rx0 + 1, e.image.cols);
        const int ry0 = clamp_int(std::floor((dy0 - sy0) * full_ky), 0, e.image.rows - 1);
        const int ry1 = clamp_int(std::ceil((dy1 - sy0) * full_ky), ry0 + 1, e.image.rows);
        const cv::Mat roi = e.image(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));
        if (roi.type() == CV_8UC3) {
            cv::resize(roi, dst, dst_rect.size(), 0, 0, cv::INTER_LINEAR);
        } else {
            cv::Mat small;
            cv::resize(resize_supports(roi.depth()) ? roi : to_bgr8(roi), small, dst_rect.size(), 0,
                       0, cv::INTER_LINEAR);
            to_bgr8(small).copyTo(dst);
        }
        return true;
    }

    // Pick a cached level so we never read far more pixels than we draw.
    const double kmin = std::min(e.image.cols / (sx1 - sx0), e.image.rows / (sy1 - sy0));
    int level = kmin >= 2.0 ? static_cast<int>(std::floor(std::log2(kmin))) : 0;
    while (level > 0 && ((e.image.cols >> level) < 2 || (e.image.rows >> level) < 2))
        --level;
    const cv::Mat& src = level_image(e, level);

    // Source pixels per screen pixel at this level, and the source span the destination covers.
    const double kx = src.cols / (sx1 - sx0), ky = src.rows / (sy1 - sy0);
    const double u0 = (dx0 - sx0) * kx, u1 = (dx1 - sx0) * kx;
    const double v0 = (dy0 - sy0) * ky, v1 = (dy1 - sy0) * ky;

    if (kx >= 1.0 && ky >= 1.0) {
        // Shrinking: area-average the visible source region straight into the canvas.
        const int rx0 = clamp_int(std::floor(u0), 0, src.cols - 1);
        const int rx1 = clamp_int(std::ceil(u1), rx0 + 1, src.cols);
        const int ry0 = clamp_int(std::floor(v0), 0, src.rows - 1);
        const int ry1 = clamp_int(std::ceil(v1), ry0 + 1, src.rows);
        const cv::Mat roi = src(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));
        if (roi.type() == CV_8UC3) {
            cv::resize(roi, dst, dst_rect.size(), 0, 0, cv::INTER_AREA);
        } else {
            cv::Mat small;
            cv::resize(resize_supports(roi.depth()) ? roi : to_bgr8(roi), small, dst_rect.size(), 0,
                       0, cv::INTER_AREA);
            to_bgr8(small).copyTo(dst);
        }
        return false;
    }

    // Enlarging: warp the visible source region (plus a 1 px margin) with sub-pixel accuracy.
    const int rx0 = clamp_int(std::floor(u0) - 1, 0, src.cols - 1);
    const int rx1 = clamp_int(std::ceil(u1) + 1, rx0 + 1, src.cols);
    const int ry0 = clamp_int(std::floor(v0) - 1, 0, src.rows - 1);
    const int ry1 = clamp_int(std::ceil(v1) + 1, ry0 + 1, src.rows);
    const cv::Mat roi = to_bgr8(src(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0)));

    // ROI pixel x (centre at x + 0.5 in edge coordinates) -> destination pixel index.
    const double ax = 1.0 / kx, ay = 1.0 / ky;
    const cv::Mat m = (cv::Mat_<double>(2, 3) << ax, 0.0, (rx0 + 0.5) * ax + sx0 - dx0 - 0.5, 0.0,
                       ay, (ry0 + 0.5) * ay + sy0 - dy0 - 0.5);
    // Zoomed in far enough to see individual pixels: show them crisp for inspection.
    const int interpolation = (ax >= 3.0 && ay >= 3.0) ? cv::INTER_NEAREST : cv::INTER_LINEAR;
    cv::warpAffine(roi, dst, m, dst_rect.size(), interpolation, cv::BORDER_REPLICATE);
    return false;
}

/**
 * Draw a rotated frame, leaving canvas pixels outside it untouched. Return true if the fast
 * (live) shrinking method was used.
 */
static bool draw_rotated_frame(cv::Mat& canvas,
                               FrameEntry& e,
                               const Camera& cam,
                               ClockType::time_point now) {
    // Destination: the screen bounding box of the rotated frame, clipped to the canvas.
    const cv::Rect2d b = e.bounds();
    const int dx0 = clamp_int(std::floor((b.x - cam.offset.x) * cam.zoom), 0, canvas.cols);
    const int dx1 = clamp_int(std::ceil((b.x + b.width - cam.offset.x) * cam.zoom), 0, canvas.cols);
    const int dy0 = clamp_int(std::floor((b.y - cam.offset.y) * cam.zoom), 0, canvas.rows);
    const int dy1 =
        clamp_int(std::ceil((b.y + b.height - cam.offset.y) * cam.zoom), 0, canvas.rows);
    if (dx1 <= dx0 || dy1 <= dy0) return false;  // off-screen

    // Source: full resolution for a live frame being shrunk, otherwise a cached level picked
    // like draw_frame() does, so we never read far more pixels than we draw.
    const cv::Rect2d& wr = e.world_rect;
    const double frame_w = wr.width * cam.zoom, frame_h = wr.height * cam.zoom;  // screen px
    const bool live = e.image.cols >= frame_w && e.image.rows >= frame_h && e.is_live(now);
    int level = 0;
    if (!live) {
        const double kmin = std::min(e.image.cols / frame_w, e.image.rows / frame_h);
        level = kmin >= 2.0 ? static_cast<int>(std::floor(std::log2(kmin))) : 0;
        while (level > 0 && ((e.image.cols >> level) < 2 || (e.image.rows >> level) < 2))
            --level;
    }
    const cv::Mat& src = level_image(e, level);

    // Source edge coordinates (u, v) -> screen: screen = A * (u, v) + t, where A = R * scale
    // and t is the screen position of the rotated top-left corner.
    const double rad = e.angle_deg * CV_PI / 180.0;
    const double cos_a = std::cos(rad), sin_a = std::sin(rad);
    const double px = frame_w / src.cols, py = frame_h / src.rows;  // screen px per source px
    const double a00 = cos_a * px, a01 = -sin_a * py, a10 = sin_a * px, a11 = cos_a * py;
    const double hw = wr.width / 2.0, hh = wr.height / 2.0;
    const double tx = (wr.x + hw - hw * cos_a + hh * sin_a - cam.offset.x) * cam.zoom;
    const double ty = (wr.y + hh - hw * sin_a - hh * cos_a - cam.offset.y) * cam.zoom;

    // Crop the source to what the destination box maps back to (plus a 1 px margin), so the
    // cost follows the window size, not the frame size.
    double u_min = HUGE_VAL, u_max = -HUGE_VAL, v_min = HUGE_VAL, v_max = -HUGE_VAL;
    for (const cv::Point2d s : {cv::Point2d(dx0, dy0), cv::Point2d(dx1, dy0), cv::Point2d(dx0, dy1),
                                cv::Point2d(dx1, dy1)}) {
        const double x = s.x - tx, y = s.y - ty;
        const double u = (cos_a * x + sin_a * y) / px, v = (-sin_a * x + cos_a * y) / py;
        u_min = std::min(u_min, u);
        u_max = std::max(u_max, u);
        v_min = std::min(v_min, v);
        v_max = std::max(v_max, v);
    }
    const int rx0 = clamp_int(std::floor(u_min) - 1, 0, src.cols - 1);
    const int rx1 = clamp_int(std::ceil(u_max) + 1, rx0 + 1, src.cols);
    const int ry0 = clamp_int(std::floor(v_min) - 1, 0, src.rows - 1);
    const int ry1 = clamp_int(std::ceil(v_max) + 1, ry0 + 1, src.rows);
    cv::Mat roi = src(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));

    // Shrinking: resize to about screen scale first. Reading rows in order is far cheaper than
    // the diagonal reads of a rotating warp over a large image.
    double fx = 1.0, fy = 1.0;  // source px per roi px
    if (px < 1.0 || py < 1.0) {
        const cv::Size size(std::max(1, static_cast<int>(std::ceil(roi.cols * std::min(px, 1.0)))),
                            std::max(1, static_cast<int>(std::ceil(roi.rows * std::min(py, 1.0)))));
        cv::Mat shrunk;
        cv::resize(resize_supports(roi.depth()) ? roi : to_bgr8(roi), shrunk, size, 0, 0,
                   live ? cv::INTER_LINEAR : cv::INTER_AREA);
        fx = static_cast<double>(roi.cols) / size.width;
        fy = static_cast<double>(roi.rows) / size.height;
        roi = shrunk;
    }
    roi = to_bgr8(roi);

    // ROI pixel (i, j), centred at source (rx0 + (i + 0.5) fx, ry0 + (j + 0.5) fy) ->
    // destination pixel index.
    const double u0 = rx0 + 0.5 * fx, v0 = ry0 + 0.5 * fy;
    const cv::Mat m =
        (cv::Mat_<double>(2, 3) << a00 * fx, a01 * fy, a00 * u0 + a01 * v0 + tx - dx0 - 0.5,
         a10 * fx, a11 * fy, a10 * u0 + a11 * v0 + ty - dy0 - 0.5);
    const cv::Size dst_size(dx1 - dx0, dy1 - dy0);
    const int interpolation = (px >= 3.0 && py >= 3.0) ? cv::INTER_NEAREST : cv::INTER_LINEAR;
    cv::Mat warped, mask;
    cv::warpAffine(roi, warped, m, dst_size, interpolation, cv::BORDER_REPLICATE);
    // Coverage: a destination pixel belongs to the frame if its centre maps inside the image.
    // The crop margin means it can only map outside the ROI where the ROI meets the image edge.
    cv::warpAffine(cv::Mat(roi.size(), CV_8U, cv::Scalar(255)), mask, m, dst_size,
                   cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
    warped.copyTo(canvas(cv::Rect(dx0, dy0, dst_size.width, dst_size.height)), mask);
    return live;
}

}  // namespace fv::detail
