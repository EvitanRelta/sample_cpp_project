/**
 * Internal state shared between a fv::Viewer handle (any thread) and its widget on the UI thread;
 * lives in a shared_ptr so queued UI work can outlive the handle.
 */
#pragma once

#include <QPointer>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "fv/viewer.h"

namespace fv::detail {

using ClockType = std::chrono::steady_clock;

/** Reserved id used by show() / show_shared(). */
inline constexpr FrameId MAIN_FRAME_ID = ~FrameId{0};

/**
 * Holds downscaled copies of one version of a frame (level n = 1 / 2^n size). Created fresh for
 * every new image, and only ever read or written on the UI thread.
 */
struct LevelCache {
    std::map<int, cv::Mat> levels;
};

struct FrameEntry {
    cv::Mat image;  // owned copy, or the caller's shared buffer
    // Immutable, so a paint's snapshot can share it; replaced together with the image.
    std::shared_ptr<const std::vector<Box>> boxes;
    cv::Rect2d world_rect;
    double angle_deg = 0.0;  // clockwise on screen, about the world_rect centre
    int layer = 0;
    std::uint64_t order = 0;  // first-insertion order, for stable draw order within a layer
    std::shared_ptr<LevelCache> cache = std::make_shared<LevelCache>();
    ClockType::time_point last_update{};  // when this image was submitted
    ClockType::duration update_interval = ClockType::duration::max();  // gap to the previous update
    bool drawn = false;  // a paint has shown this image (for the replaced / latency stats)

    /**
     * Return whether the frame is "live", i.e. being replaced repeatedly (video, streaming). Live
     * frames are shrunk with a fast method on every paint instead of building cached levels that
     * would be thrown away.
     */
    bool is_live(ClockType::time_point now) const {
        const auto window = std::chrono::milliseconds(500);
        return update_interval < window && now - last_update < window;
    }

    /** Return the axis-aligned world rect covering the frame, including its rotation. */
    cv::Rect2d bounds() const {
        if (angle_deg == 0.0) return world_rect;
        const double rad = angle_deg * CV_PI / 180.0;
        const double c = std::abs(std::cos(rad)), s = std::abs(std::sin(rad));
        const double w = world_rect.width * c + world_rect.height * s;
        const double h = world_rect.width * s + world_rect.height * c;
        return {world_rect.x + (world_rect.width - w) / 2.0,
                world_rect.y + (world_rect.height - h) / 2.0, w, h};
    }
};

struct Camera {
    double zoom = 1.0;             // screen pixels per world unit
    cv::Point2d offset{0.0, 0.0};  // world coordinates of the window's top-left corner
    cv::Size screen_size{0, 0};

    cv::Point2d to_world(cv::Point2d s) const {
        return {offset.x + s.x / zoom, offset.y + s.y / zoom};
    }
    cv::Point2d to_screen(cv::Point2d w) const {
        return {(w.x - offset.x) * zoom, (w.y - offset.y) * zoom};
    }

    fv::Viewport as_viewport() const {
        fv::Viewport v;
        v.world_rect = {offset.x, offset.y, screen_size.width / zoom, screen_size.height / zoom};
        v.world_per_screen_px = 1.0 / zoom;
        v.screen_size = screen_size;
        return v;
    }
};

/** Set cam so that `bounds` fits the screen, centred, preserving aspect ratio. */
inline void fit_camera(Camera& cam, const cv::Rect2d& bounds) {
    if (cam.screen_size.width <= 0 || cam.screen_size.height <= 0) return;
    if (bounds.width <= 0 || bounds.height <= 0) return;
    cam.zoom =
        std::min(cam.screen_size.width / bounds.width, cam.screen_size.height / bounds.height);
    cam.offset.x = bounds.x + bounds.width / 2.0 - cam.screen_size.width / (2.0 * cam.zoom);
    cam.offset.y = bounds.y + bounds.height / 2.0 - cam.screen_size.height / (2.0 * cam.zoom);
}

/** Timestamped samples from the last second, for fv::ViewerStats. */
struct StatSamples {
    std::deque<std::pair<ClockType::time_point, double>> samples;

    /** Add a sample and drop expired ones, so the window stays bounded without a reader. */
    void add(ClockType::time_point now, double value = 0.0) {
        samples.emplace_back(now, value);
        prune(now);
    }
    /** Drop samples older than one second before `now`. */
    void prune(ClockType::time_point now) {
        while (!samples.empty() && now - samples.front().first > std::chrono::seconds(1))
            samples.pop_front();
    }
    /** Return the sample rate per second, from the span between the first and last sample. */
    double rate() const {
        if (samples.size() < 2) return static_cast<double>(samples.size());
        const double span =
            std::chrono::duration<double>(samples.back().first - samples.front().first).count();
        return span > 0 ? (samples.size() - 1) / span : 0.0;
    }
    double avg() const {
        double sum = 0.0;
        for (const auto& [time, value] : samples)
            sum += value;
        return samples.empty() ? 0.0 : sum / samples.size();
    }
    double max() const {
        double result = 0.0;
        for (const auto& [time, value] : samples)
            result = std::max(result, value);
        return result;
    }
};

struct Binding {
    Key key;
    std::string description;
    std::function<void()> action;
};

struct OverlayItem {
    std::string text;
    Qt::Corner corner = Qt::TopLeftCorner;
    cv::Scalar colour;  // BGR
    std::uint64_t order = 0;
};

struct OverlayBox {
    Box box;  // rect in world coordinates
    std::uint64_t order = 0;
};

struct OverlayPolygon {
    std::vector<cv::Point2d> world_pts;
    cv::Scalar colour;  // BGR
    int thickness_px = 2;
    double fill_opacity = 0.0;
    bool closed = true;
    std::uint64_t order = 0;
};

struct QueuedEvent {
    enum class Kind : std::uint8_t { KEY, MOUSE } type = Kind::KEY;
    Key key;
    fv::MouseEvent mouse;
};

class ViewerState : public std::enable_shared_from_this<ViewerState> {
   public:
    // ---- Guarded by mutex ----------------------------------------------------------------
    // One lock for frames, overlays and camera, so a batch() applies within one hold and a paint
    // snapshots all of them within one hold. Held only for a few microseconds at a time: painting
    // copies a snapshot (cheap Mat headers and immutable box-list pointers) and draws from that,
    // so submitting a frame never waits for a paint.
    mutable std::mutex mutex;
    std::condition_variable events_cv;

    std::map<FrameId, FrameEntry> frames;
    std::uint64_t next_order = 0;
    std::uint64_t frames_version = 0;  // bumped on every change
    bool warned_same_buffer = false;   // debug warning, printed once

    Camera cam;
    bool auto_fit = true;  // keep fitting content until the user or app moves the camera

    std::deque<QueuedEvent> events;
    std::optional<fv::Viewport> pending_viewport;
    std::optional<fv::Viewport> last_notified_viewport;
    bool closed = false;

    std::vector<Binding> bindings;
    std::function<void(const fv::MouseEvent&)> mouse_callback;
    std::function<void(const fv::Viewport&)> viewport_callback;

    std::map<std::string, OverlayItem> overlays;
    std::map<std::string, OverlayBox> overlay_boxes;
    std::map<std::string, OverlayPolygon> overlay_polygons;
    std::uint64_t next_overlay_order = 0;  // shared by text, box and polygon overlays
    bool fps_enabled = false;
    // Drawing stats, always collected (see fv::ViewerStats for what each counts)
    StatSamples submitted_times;
    StatSamples drawn_times;
    StatSamples replaced_times;
    StatSamples paint_ms;
    StatSamples latency_ms;

    // ---- UI thread only ----------------------------------------------------------------
    QPointer<QWidget> widget;
    std::atomic<bool> repaint_pending{false};

    /** Schedule one repaint (coalesced). Any thread. */
    void request_repaint();

    /**
     * Queue a viewport-changed event if the camera differs from the last one reported. Caller
     * holds `mutex`.
     *
     * @return true if queued (then notify events_cv after unlocking)
     */
    bool queue_viewport_locked() {
        const fv::Viewport v = cam.as_viewport();
        if (last_notified_viewport) {
            const fv::Viewport& l = *last_notified_viewport;
            if (l.world_rect == v.world_rect && l.screen_size == v.screen_size &&
                l.world_per_screen_px == v.world_per_screen_px)
                return false;
        }
        last_notified_viewport = v;
        pending_viewport = v;
        return true;
    }

    /** Return the drawing stats over the second before `now`. Caller holds `mutex`. */
    fv::ViewerStats stats_locked(ClockType::time_point now) {
        for (StatSamples* s :
             {&submitted_times, &drawn_times, &replaced_times, &paint_ms, &latency_ms})
            s->prune(now);
        fv::ViewerStats st;
        st.submitted_per_s = submitted_times.rate();
        st.drawn_per_s = drawn_times.rate();
        st.replaced_per_s = replaced_times.rate();
        st.paint_ms_avg = paint_ms.avg();
        st.paint_ms_max = paint_ms.max();
        st.latency_ms_avg = latency_ms.avg();
        st.latency_ms_max = latency_ms.max();
        return st;
    }

    /** Return whether `k` is bound by the user. Caller holds `mutex`. */
    bool is_bound_locked(Key k) const {
        for (const auto& b : bindings)
            if (b.key == k) return true;
        return false;
    }
};

}  // namespace fv::detail
