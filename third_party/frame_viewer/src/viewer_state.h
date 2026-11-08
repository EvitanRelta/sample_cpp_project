// viewer_state.h — internal state shared between a fv::Viewer handle (any thread) and its
// widget on the UI thread. Lives in a shared_ptr, so queued UI work can outlive the handle.
#pragma once

#include "fv/viewer.h"

#include <QPointer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
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

namespace fv::detail {

using clock_type = std::chrono::steady_clock;

// Reserved id used by show() / show_shared().
inline constexpr FrameId main_frame_id = ~FrameId{0};

// Downscaled copies of one version of a frame (level n = 1 / 2^n size). Created fresh for every
// new image, and only ever read or written on the UI thread.
struct level_cache {
    std::map<int, cv::Mat> levels;
};

struct frame_entry {
    cv::Mat image;                   // owned copy, or the caller's shared buffer
    cv::Rect2d world_rect;
    int layer = 0;
    std::uint64_t order = 0;         // first-insertion order, for stable draw order within a layer
    std::shared_ptr<level_cache> cache = std::make_shared<level_cache>();
    clock_type::time_point last_update{};
    clock_type::duration update_interval = clock_type::duration::max(); // gap to the previous update

    // "Live" = being replaced repeatedly (video, streaming). Live frames are shrunk with a fast
    // method on every paint instead of building cached levels that would be thrown away.
    bool is_live(clock_type::time_point now) const {
        const auto window = std::chrono::milliseconds(500);
        return update_interval < window && now - last_update < window;
    }
};

struct camera {
    double zoom = 1.0;               // screen pixels per world unit
    cv::Point2d offset{0.0, 0.0};    // world coordinates of the window's top-left corner
    cv::Size screen_size{0, 0};

    cv::Point2d to_world(cv::Point2d s) const { return {offset.x + s.x / zoom, offset.y + s.y / zoom}; }
    cv::Point2d to_screen(cv::Point2d w) const { return {(w.x - offset.x) * zoom, (w.y - offset.y) * zoom}; }

    fv::Viewport as_viewport() const {
        fv::Viewport v;
        v.world_rect = {offset.x, offset.y, screen_size.width / zoom, screen_size.height / zoom};
        v.world_per_screen_px = 1.0 / zoom;
        v.screen_size = screen_size;
        return v;
    }
};

// Sets cam so that `bounds` fits the screen, centred, preserving aspect ratio.
inline void fit_camera(camera& cam, const cv::Rect2d& bounds) {
    if (cam.screen_size.width <= 0 || cam.screen_size.height <= 0) return;
    if (bounds.width <= 0 || bounds.height <= 0) return;
    cam.zoom = std::min(cam.screen_size.width / bounds.width, cam.screen_size.height / bounds.height);
    cam.offset.x = bounds.x + bounds.width / 2.0 - cam.screen_size.width / (2.0 * cam.zoom);
    cam.offset.y = bounds.y + bounds.height / 2.0 - cam.screen_size.height / (2.0 * cam.zoom);
}

struct binding {
    Key key;
    std::string description;
    std::function<void()> action;
};

struct overlay_item {
    std::string text;
    Qt::Corner corner = Qt::TopLeftCorner;
    std::uint64_t order = 0;
};

struct overlay_rect {
    cv::Rect2d world_rect;
    cv::Scalar colour; // BGR
    int thickness_px = 2;
    double fill_opacity = 0.0;
    std::uint64_t order = 0;
};

struct queued_event {
    enum class kind { key, mouse } type = kind::key;
    Key key;
    fv::MouseEvent mouse;
};

class ViewerState : public std::enable_shared_from_this<ViewerState> {
public:
    // ---- Frames: guarded by frames_mutex --------------------------------------------------
    // Held only for a few microseconds at a time: painting copies a snapshot of the entries
    // (cheap Mat headers) and draws from that, so submitting a frame never waits for a paint.
    std::mutex frames_mutex;
    std::map<FrameId, frame_entry> frames;
    std::uint64_t next_order = 0;
    std::uint64_t frames_version = 0;             // bumped on every change
    clock_type::time_point latest_submit_time{};  // when the newest content was submitted
    bool warned_same_buffer = false;              // debug warning, printed once

    // ---- Everything else: guarded by mutex (short holds only) ---------------------------
    mutable std::mutex mutex;
    std::condition_variable events_cv;

    camera cam;
    bool auto_fit = true; // keep fitting content until the user or app moves the camera

    std::deque<queued_event> events;
    std::optional<fv::Viewport> pending_viewport;
    std::optional<fv::Viewport> last_notified_viewport;
    bool closed = false;

    std::vector<binding> bindings;
    std::function<void(const fv::MouseEvent&)> mouse_callback;
    std::function<void(const fv::Viewport&)> viewport_callback;

    std::map<std::string, overlay_item> overlays;
    std::map<std::string, overlay_rect> overlay_rects;
    std::uint64_t next_overlay_order = 0; // shared by text and rect overlays
    bool fps_enabled = false;
    std::deque<clock_type::time_point> frame_times;  // show()/show_shared() calls
    std::deque<clock_type::time_point> shown_times;  // paints that displayed new content
    std::deque<std::pair<clock_type::time_point, double>> latencies_ms; // submit -> painted

    // ---- UI thread only ----------------------------------------------------------------
    QPointer<QWidget> widget;
    std::atomic<bool> repaint_pending{false};

    // Any thread: schedule one repaint (coalesced).
    void request_repaint();

    // Caller holds `mutex`. Queues a viewport-changed event if the camera differs from the
    // last one reported. Returns true if queued (then notify events_cv after unlocking).
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

    // Caller holds `mutex`. Returns true if `k` is bound by the user.
    bool is_bound_locked(Key k) const {
        for (const auto& b : bindings)
            if (b.key == k) return true;
        return false;
    }
};

} // namespace fv::detail
