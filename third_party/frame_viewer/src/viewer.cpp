#include "fv/viewer.h"
#include <QGuiApplication>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>
#include "key_utils.h"
#include "ui_thread.h"
#include "view_widget.h"
#include "viewer_state.h"

namespace fv {

using detail::ClockType;
using detail::UiThread;
using detail::ViewerState;

namespace {

/** Work left for after the state lock is released. */
struct AfterUnlock {
    // Replaced and removed images, box lists and caches, released on the calling thread outside
    // the lock. A paint still drawing one holds its own snapshot reference, so a buffer is only
    // freed (or returned to the caller's pool) once nothing uses it. Deferring the release to the
    // UI thread instead would let buffers pile up while the UI is busy.
    std::vector<cv::Mat> images;
    std::vector<std::shared_ptr<const void>> objects;
    bool wake_owner = false;  // a viewport change was queued for wait_key()

    /** Take a frame entry's image, boxes and cache, to release them after unlocking. */
    void take(detail::FrameEntry& e) {
        images.push_back(std::move(e.image));
        objects.push_back(std::move(e.boxes));
        objects.push_back(std::move(e.cache));
    }
};

/** A change to frames, overlays or the camera; runs with `mutex` held, at time `now`. */
using Change = std::function<void(ViewerState& s, ClockType::time_point now, AfterUnlock& after)>;

/** A batch() open on this thread for one viewer. */
struct OpenBatch {
    const ViewerState* state = nullptr;
    int depth = 0;                // nesting level; nested batches join the outermost one
    std::vector<Change> changes;  // staged in call order
};

}  // namespace

// Forward declarations of helper functions
static OpenBatch* find_batch(const ViewerState& s);
static void apply_change(ViewerState& s, Change change);
static void apply_changes(ViewerState& s, std::vector<Change>& changes);

// Batches open on this thread, at most one per viewer. Only this thread stages into them, so
// calls from other threads never join a batch and never wait for one.
static thread_local std::vector<OpenBatch> open_batches;

void detail::ViewerState::request_repaint() {
    if (repaint_pending.exchange(true)) return;  // one repaint already queued; it'll see our change
    auto self = shared_from_this();
    UiThread::instance().post([self] {
        self->repaint_pending = false;
        if (self->widget) self->widget->update();
    });
}

// ---- Lifetime ------------------------------------------------------------------------------

Viewer::Viewer(std::string title, cv::Size size) : m_state(std::make_shared<ViewerState>()) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->cam.screen_size = size;  // provisional until the window reports its real size
    }
    UiThread::instance().viewer_created();
    auto s = m_state;
    UiThread::instance().post([s, title = std::move(title), size] {
        auto* w = new detail::ViewWidget(s);
        s->widget = w;
        w->setWindowTitle(QString::fromStdString(title));
        w->resize(size.width, size.height);
        w->show();
        w->raise();
        // Wayland lets only the compositor move focus; asking just logs a warning
        if (QGuiApplication::platformName() != QLatin1String("wayland")) w->activateWindow();
    });
}

Viewer::~Viewer() {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->closed = true;
    }
    m_state->events_cv.notify_all();
    // Delete the window synchronously, so no UI work for this viewer is still in flight when
    // the program exits.
    auto s = std::move(m_state);
    UiThread::instance().post_and_wait([s] {
        if (s->widget) delete s->widget.data();
    });
    UiThread::instance().viewer_destroyed();
}

// ---- Frames --------------------------------------------------------------------------------

void Viewer::put_frame(FrameId id,
                       cv::Mat image,
                       const cv::Rect2d& world_rect,
                       int layer,
                       double angle_deg,
                       bool shared,
                       std::vector<Box> boxes) {
    // Allocate here, outside the lock; paints only copy the immutable list's shared pointer.
    std::shared_ptr<const std::vector<Box>> submitted_boxes;
    if (!boxes.empty())
        submitted_boxes = std::make_shared<const std::vector<Box>>(std::move(boxes));
    auto cache = std::make_shared<detail::LevelCache>();
    apply_change(*m_state, [id, image = std::move(image), world_rect, layer, angle_deg, shared,
                            boxes = std::move(submitted_boxes),
                            cache = std::move(cache)](ViewerState& s, ClockType::time_point now,
                                                      AfterUnlock& after) mutable {
        auto [it, inserted] = s.frames.try_emplace(id);
        detail::FrameEntry& e = it->second;
        if (inserted) e.order = s.next_order++;
#ifndef NDEBUG
        if (shared && !inserted && !s.warned_same_buffer && !image.empty() &&
            e.image.data == image.data) {
            s.warned_same_buffer = true;
            std::fprintf(
                stderr,
                "fv::Viewer warning: show_shared()/set_frame_shared() was given the buffer it is "
                "already showing. Writing into a shown buffer can tear; take each new buffer "
                "from a fv::BufferPool.\n");
        }
#else
        (void)shared;
#endif
        s.submitted_times.add(now);
        if (!inserted && !e.drawn) s.replaced_times.add(now);  // newest wins over an undrawn one
        after.take(e);  // paints holding the old image or cache keep them alive
        e.image = std::move(image);
        e.boxes = std::move(boxes);
        e.cache = std::move(cache);
        e.world_rect = world_rect;
        e.angle_deg = angle_deg;
        e.layer = layer;
        e.update_interval = inserted ? ClockType::duration::max() : now - e.last_update;
        e.last_update = now;
        e.drawn = false;
        ++s.frames_version;
    });
}

void Viewer::show(const cv::Mat& image) {
    put_frame(detail::MAIN_FRAME_ID, image.clone(), cv::Rect2d(0, 0, image.cols, image.rows), 0,
              0.0, false);
}

void Viewer::show(const cv::Mat& image, std::vector<Box> boxes) {
    put_frame(detail::MAIN_FRAME_ID, image.clone(), cv::Rect2d(0, 0, image.cols, image.rows), 0,
              0.0, false, std::move(boxes));
}

void Viewer::show_shared(const cv::Mat& image) {
    put_frame(detail::MAIN_FRAME_ID, image, cv::Rect2d(0, 0, image.cols, image.rows), 0, 0.0, true);
}

void Viewer::show_shared(const cv::Mat& image, std::vector<Box> boxes) {
    put_frame(detail::MAIN_FRAME_ID, image, cv::Rect2d(0, 0, image.cols, image.rows), 0, 0.0, true,
              std::move(boxes));
}

void Viewer::set_frame(FrameId id,
                       const cv::Mat& image,
                       const cv::Rect2d& world_rect,
                       int layer,
                       double angle_deg) {
    put_frame(id, image.clone(), world_rect, layer, angle_deg, false);
}

void Viewer::set_frame_shared(FrameId id,
                              const cv::Mat& image,
                              const cv::Rect2d& world_rect,
                              int layer,
                              double angle_deg) {
    put_frame(id, image, world_rect, layer, angle_deg, true);
}

void Viewer::remove_frame(FrameId id) {
    apply_change(*m_state, [id](ViewerState& s, ClockType::time_point, AfterUnlock& after) {
        auto it = s.frames.find(id);
        if (it == s.frames.end()) return;
        after.take(it->second);
        s.frames.erase(it);
        ++s.frames_version;
    });
}

void Viewer::move_frame(FrameId id,
                        const cv::Rect2d& world_rect,
                        std::optional<int> layer,
                        std::optional<double> angle_deg) {
    apply_change(*m_state, [=](ViewerState& s, ClockType::time_point, AfterUnlock&) {
        auto it = s.frames.find(id);
        if (it == s.frames.end()) return;
        // Image, cache and live status are untouched, and frames_version isn't bumped: a move
        // isn't new content, so it doesn't count as drawn content or a submission in stats().
        it->second.world_rect = world_rect;
        if (layer) it->second.layer = *layer;
        if (angle_deg) it->second.angle_deg = *angle_deg;
    });
}

void Viewer::clear_frames() {
    apply_change(*m_state, [](ViewerState& s, ClockType::time_point, AfterUnlock& after) {
        for (auto& [id, e] : s.frames)
            after.take(e);
        s.frames.clear();
        ++s.frames_version;
    });
}

void Viewer::batch(const std::function<void()>& changes) {
    ViewerState& s = *m_state;
    if (OpenBatch* open = find_batch(s))
        ++open->depth;  // nested: join the outer batch
    else
        open_batches.push_back({&s, 1, {}});
    const std::size_t staged_before = find_batch(s)->changes.size();

    std::exception_ptr error;
    try {
        changes();
    } catch (...) {
        error = std::current_exception();
    }

    // Find it again: batches opened on other viewers inside `changes` may have moved it.
    OpenBatch* open = find_batch(s);
    if (error) open->changes.erase(open->changes.begin() + staged_before, open->changes.end());
    std::vector<Change> staged;
    if (--open->depth == 0) {
        staged = std::move(open->changes);
        open_batches.erase(open_batches.begin() + (open - open_batches.data()));
    }
    if (error) std::rethrow_exception(error);
    if (!staged.empty()) apply_changes(s, staged);
}

// ---- Waiting and events --------------------------------------------------------------------

Key Viewer::wait_key(int timeout_ms) {
    ViewerState& s = *m_state;
    if (find_batch(s)) throw std::logic_error("fv::Viewer: wait_key() called inside batch()");
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));

    std::unique_lock<std::mutex> lock(s.mutex);
    for (;;) {
        // Dispatch everything that's waiting. Callbacks run without the lock held.
        for (;;) {
            if (s.pending_viewport) {
                const Viewport v = *s.pending_viewport;
                s.pending_viewport.reset();
                auto cb = s.viewport_callback;
                if (cb) {
                    lock.unlock();
                    cb(v);
                    lock.lock();
                }
                continue;
            }
            if (s.events.empty()) break;

            detail::QueuedEvent ev = std::move(s.events.front());
            s.events.pop_front();

            if (ev.type == detail::QueuedEvent::Kind::MOUSE) {
                auto cb = s.mouse_callback;
                if (cb) {
                    lock.unlock();
                    cb(ev.mouse);
                    lock.lock();
                }
                continue;
            }

            std::function<void()> action;
            for (const auto& b : s.bindings)
                if (b.key == ev.key) action = b.action;
            if (!action) return Key(ev.key);
            lock.unlock();
            action();
            lock.lock();
        }

        if (s.closed) return {};
        if (timeout_ms >= 0) {
            if (Clock::now() >= deadline) return {};
            s.events_cv.wait_until(lock, deadline);
        } else {
            s.events_cv.wait(lock);
        }
    }
}

void Viewer::wait_until_closed() {
    if (find_batch(*m_state))
        throw std::logic_error("fv::Viewer: wait_until_closed() called inside batch()");
    while (is_open())
        wait_key(-1);
}

bool Viewer::is_open() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return !m_state->closed;
}

void Viewer::close() {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->closed = true;
    }
    m_state->events_cv.notify_all();
    auto s = m_state;
    UiThread::instance().post([s] {
        if (s->widget) s->widget->close();
    });
}

// ---- Callbacks -----------------------------------------------------------------------------

void Viewer::bind(Key k, std::string description, std::function<void()> action) {
    k = detail::normalize_key(k);
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        for (auto& b : m_state->bindings) {
            if (b.key == k) {
                b.description = std::move(description);
                b.action = std::move(action);
                m_state->request_repaint();
                return;
            }
        }
        m_state->bindings.push_back({k, std::move(description), std::move(action)});
    }
    m_state->request_repaint();  // the help panel may be open
}

void Viewer::bind(std::string_view key_text,
                  std::string description,
                  std::function<void()> action) {
    bind(detail::parse_key(key_text), std::move(description), std::move(action));
}

void Viewer::on_mouse(std::function<void(const MouseEvent&)> callback) {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->mouse_callback = std::move(callback);
}

void Viewer::on_viewport_changed(std::function<void(const Viewport&)> callback) {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->viewport_callback = std::move(callback);
}

// ---- Overlays ------------------------------------------------------------------------------

void Viewer::set_overlay_text(const std::string& id,
                              std::string text,
                              Qt::Corner corner,
                              cv::Scalar colour) {
    apply_change(*m_state, [id, text = std::move(text), corner, colour](
                               ViewerState& s, ClockType::time_point, AfterUnlock&) mutable {
        auto [it, inserted] = s.overlays.try_emplace(id);
        if (inserted) it->second.order = s.next_overlay_order++;
        it->second.text = std::move(text);
        it->second.corner = corner;
        it->second.colour = colour;
    });
}

void Viewer::set_overlay_box(const std::string& id, const Box& box) {
    apply_change(*m_state, [id, box](ViewerState& s, ClockType::time_point, AfterUnlock&) {
        auto [it, inserted] = s.overlay_boxes.try_emplace(id);
        if (inserted) it->second.order = s.next_overlay_order++;
        it->second.box = box;
    });
}

void Viewer::set_overlay_rect(const std::string& id,
                              const cv::Rect2d& world_rect,
                              cv::Scalar colour,
                              int thickness_px,
                              double fill_opacity) {
    set_overlay_box(id, Box{world_rect, colour, thickness_px, {}, fill_opacity});
}

void Viewer::set_overlay_polygon(const std::string& id,
                                 std::vector<cv::Point2d> world_pts,
                                 cv::Scalar colour,
                                 int thickness_px,
                                 double fill_opacity,
                                 bool closed) {
    apply_change(*m_state,
                 [id, world_pts = std::move(world_pts), colour, thickness_px, fill_opacity, closed](
                     ViewerState& s, ClockType::time_point, AfterUnlock&) mutable {
                     auto [it, inserted] = s.overlay_polygons.try_emplace(id);
                     if (inserted) it->second.order = s.next_overlay_order++;
                     it->second.world_pts = std::move(world_pts);
                     it->second.colour = colour;
                     it->second.thickness_px = std::max(thickness_px, 0);
                     it->second.fill_opacity = std::clamp(fill_opacity, 0.0, 1.0);
                     it->second.closed = closed;
                 });
}

void Viewer::remove_overlay(const std::string& id) {
    apply_change(*m_state, [id](ViewerState& s, ClockType::time_point, AfterUnlock&) {
        s.overlays.erase(id);
        s.overlay_boxes.erase(id);
        s.overlay_polygons.erase(id);
    });
}

void Viewer::show_fps(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->fps_enabled = enabled;
    }
    m_state->request_repaint();
}

ViewerStats Viewer::stats() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->stats_locked(ClockType::now());
}

// ---- Camera --------------------------------------------------------------------------------

Viewport Viewer::current_viewport() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->cam.as_viewport();
}

void Viewer::fit_all() {
    // The next paint fits and reports the new viewport.
    apply_change(*m_state,
                 [](ViewerState& s, ClockType::time_point, AfterUnlock&) { s.auto_fit = true; });
}

void Viewer::show_world_rect(const cv::Rect2d& world_rect) {
    apply_change(*m_state, [world_rect](ViewerState& s, ClockType::time_point, AfterUnlock& after) {
        detail::fit_camera(s.cam, world_rect);
        s.auto_fit = false;
        if (s.queue_viewport_locked()) after.wake_owner = true;
    });
}

cv::Point2d Viewer::screen_to_world(cv::Point2d screen_pos) const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->cam.to_world(screen_pos);
}

cv::Point2d Viewer::world_to_screen(cv::Point2d world_pos) const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->cam.to_screen(world_pos);
}

void Viewer::set_title(const std::string& title) {
    auto s = m_state;
    UiThread::instance().post([s, t = QString::fromStdString(title)] {
        if (s->widget) s->widget->setWindowTitle(t);
    });
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
/** Return this thread's open batch for `s`, or null. */
static OpenBatch* find_batch(const ViewerState& s) {
    for (OpenBatch& open : open_batches)
        if (open.state == &s) return &open;
    return nullptr;
}

/** Stage `change` in this thread's open batch for `s`, or apply it now. */
static void apply_change(ViewerState& s, Change change) {
    if (OpenBatch* open = find_batch(s)) {
        open->changes.push_back(std::move(change));
        return;
    }
    std::vector<Change> changes;
    changes.push_back(std::move(change));
    apply_changes(s, changes);
}

/**
 * Apply `changes` in order within one hold of `mutex`, so no paint (which snapshots under the
 * same lock) sees only some of them, then request a single repaint.
 */
static void apply_changes(ViewerState& s, std::vector<Change>& changes) {
    AfterUnlock after;  // destroyed at return, outside the lock
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto now = ClockType::now();
        for (Change& change : changes)
            change(s, now, after);
    }
    changes.clear();  // drop the captured images too, outside the lock
    if (after.wake_owner) s.events_cv.notify_all();
    s.request_repaint();
}

}  // namespace fv
