#include "compositor.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace fv::detail {

cv::Mat to_bgr8(const cv::Mat& in) {
    cv::Mat src = in;
    if (in.depth() != CV_8U) {
        double scale = 1.0;
        switch (in.depth()) {
        case CV_16U: scale = 1.0 / 257.0; break;
        case CV_16S: scale = 1.0 / 128.0; break;
        case CV_32F:
        case CV_64F: scale = 255.0; break;
        default: break;
        }
        in.convertTo(src, CV_8U, scale);
    }

    cv::Mat out;
    switch (src.channels()) {
    case 3: return src; // already BGR (shallow when no depth conversion happened)
    case 1: cv::cvtColor(src, out, cv::COLOR_GRAY2BGR); break;
    case 4: cv::cvtColor(src, out, cv::COLOR_BGRA2BGR); break;
    default: {
        cv::Mat first;
        cv::extractChannel(src, first, 0);
        cv::cvtColor(first, out, cv::COLOR_GRAY2BGR);
        break;
    }
    }
    return out;
}

namespace {

bool resize_supports(int depth) {
    return depth == CV_8U || depth == CV_16U || depth == CV_32F || depth == CV_64F;
}

// Returns the frame's image downscaled by 2^level, building and caching it on first use.
const cv::Mat& level_image(frame_entry& e, int level) {
    if (level <= 0) return e.image;
    auto& levels = e.cache->levels;
    auto it = levels.find(level);
    if (it != levels.end()) return it->second;

    const int div = 1 << level;
    const cv::Size size(std::max(1, (e.image.cols + div - 1) / div),
                        std::max(1, (e.image.rows + div - 1) / div));
    cv::Mat source = resize_supports(e.image.depth()) ? e.image : to_bgr8(e.image);
    cv::Mat out;
    cv::resize(source, out, size, 0, 0, cv::INTER_AREA);
    return levels.emplace(level, std::move(out)).first->second;
}

int clamp_int(double v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return static_cast<int>(v);
}

// Returns true if the fast (live) shrinking method was used.
bool draw_frame(cv::Mat& canvas, frame_entry& e, const camera& cam, clock_type::time_point now) {
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
    if (dx1 <= dx0 || dy1 <= dy0) return false; // off-screen

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
            cv::resize(resize_supports(roi.depth()) ? roi : to_bgr8(roi), small, dst_rect.size(), 0, 0,
                       cv::INTER_LINEAR);
            to_bgr8(small).copyTo(dst);
        }
        return true;
    }

    // Pick a cached level so we never read far more pixels than we draw.
    const double kmin = std::min(e.image.cols / (sx1 - sx0), e.image.rows / (sy1 - sy0));
    int level = kmin >= 2.0 ? static_cast<int>(std::floor(std::log2(kmin))) : 0;
    while (level > 0 && ((e.image.cols >> level) < 2 || (e.image.rows >> level) < 2)) --level;
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
            cv::resize(resize_supports(roi.depth()) ? roi : to_bgr8(roi), small, dst_rect.size(), 0, 0,
                       cv::INTER_AREA);
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
    const cv::Mat m = (cv::Mat_<double>(2, 3) << ax, 0.0, (rx0 + 0.5) * ax + sx0 - dx0 - 0.5,
                                                 0.0, ay, (ry0 + 0.5) * ay + sy0 - dy0 - 0.5);
    // Zoomed in far enough to see individual pixels: show them crisp for inspection.
    const int interpolation = (ax >= 3.0 && ay >= 3.0) ? cv::INTER_NEAREST : cv::INTER_LINEAR;
    cv::warpAffine(roi, dst, m, dst_rect.size(), interpolation, cv::BORDER_REPLICATE);
    return false;
}

} // namespace

bool compose(cv::Mat& canvas, std::vector<frame_entry>& frames, const camera& cam, clock_type::time_point now) {
    canvas.setTo(cv::Scalar(32, 32, 32));

    std::vector<frame_entry*> ordered;
    ordered.reserve(frames.size());
    for (auto& e : frames)
        if (!e.image.empty() && e.world_rect.width > 0 && e.world_rect.height > 0) ordered.push_back(&e);

    std::sort(ordered.begin(), ordered.end(), [](const frame_entry* a, const frame_entry* b) {
        return a->layer != b->layer ? a->layer < b->layer : a->order < b->order;
    });

    bool used_fast = false;
    for (frame_entry* e : ordered) used_fast |= draw_frame(canvas, *e, cam, now);
    return used_fast;
}

cv::Rect2d content_bounds(const std::vector<frame_entry>& frames) {
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const auto& e : frames) {
        if (e.image.empty() || e.world_rect.width <= 0 || e.world_rect.height <= 0) continue;
        const cv::Rect2d& r = e.world_rect;
        if (!any) {
            x0 = r.x; y0 = r.y; x1 = r.x + r.width; y1 = r.y + r.height;
            any = true;
        } else {
            x0 = std::min(x0, r.x); y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.x + r.width); y1 = std::max(y1, r.y + r.height);
        }
    }
    return any ? cv::Rect2d(x0, y0, x1 - x0, y1 - y0) : cv::Rect2d();
}

} // namespace fv::detail
