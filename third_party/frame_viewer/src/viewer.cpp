#include "fv/viewer.h"

#include "key_utils.h"
#include "ui_thread.h"
#include "view_widget.h"
#include "viewer_state.h"

#include <QGuiApplication>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace fv {

using detail::ui_thread;
using detail::ViewerState;

void detail::ViewerState::request_repaint() {
    if (repaint_pending.exchange(true)) return; // one repaint already queued; it'll see our change
    auto self = shared_from_this();
    ui_thread::instance().post([self] {
        self->repaint_pending = false;
        if (self->widget) self->widget->update();
    });
}

// ---- Lifetime ------------------------------------------------------------------------------

Viewer::Viewer(std::string title, cv::Size size) : m_state(std::make_shared<ViewerState>()) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->cam.screen_size = size; // provisional until the window reports its real size
    }
    ui_thread::instance().viewer_created();
    auto s = m_state;
    ui_thread::instance().post([s, title = std::move(title), size] {
        auto* w = new detail::view_widget(s);
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
    ui_thread::instance().post_and_wait([s] {
        if (s->widget) delete s->widget.data();
    });
    ui_thread::instance().viewer_destroyed();
}

// ---- Frames --------------------------------------------------------------------------------

// Replaced and removed frames are released on the calling thread as soon as they leave
// `frames`, outside frames_mutex. A paint still drawing one holds its own snapshot reference,
// so the buffer is only freed (or returned to the caller's pool) once nothing uses it. Deferring
// the release to the UI thread instead would let buffers pile up while the UI is busy.

void Viewer::put_frame(FrameId id, cv::Mat image, const cv::Rect2d& world_rect, int layer,
                       bool counts_for_fps, bool shared) {
    ViewerState& s = *m_state;
    const auto now = detail::clock_type::now();
    cv::Mat previous; // released after the lock, so freeing it never blocks a paint
    std::shared_ptr<detail::level_cache> previous_cache;
    {
        // Only a brief lock: painting works on its own snapshot, so this never waits for a paint.
        std::lock_guard<std::mutex> lock(s.frames_mutex);
        auto [it, inserted] = s.frames.try_emplace(id);
        detail::frame_entry& e = it->second;
        if (inserted) e.order = s.next_order++;
#ifndef NDEBUG
        if (shared && !inserted && !s.warned_same_buffer && !image.empty() && e.image.data == image.data) {
            s.warned_same_buffer = true;
            std::fprintf(stderr,
                         "fv::Viewer warning: show_shared()/set_frame_shared() was given the buffer it is "
                         "already showing. Writing into a shown buffer can tear; take each new buffer "
                         "from a fv::BufferPool.\n");
        }
#else
        (void)shared;
#endif
        previous = std::move(e.image);
        e.image = std::move(image);
        e.world_rect = world_rect;
        e.layer = layer;
        previous_cache = std::move(e.cache); // paints holding the old cache keep it alive
        e.cache = std::make_shared<detail::level_cache>();
        e.update_interval = inserted ? detail::clock_type::duration::max() : now - e.last_update;
        e.last_update = now;
        ++s.frames_version;
        s.latest_submit_time = now;
    }
    previous.release();
    previous_cache.reset();
    if (counts_for_fps) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.frame_times.push_back(now);
        while (s.frame_times.size() > 1000) s.frame_times.pop_front();
    }
    s.request_repaint();
}

void Viewer::show(const cv::Mat& image) {
    put_frame(detail::main_frame_id, image.clone(), cv::Rect2d(0, 0, image.cols, image.rows), 0, true, false);
}

void Viewer::show_shared(const cv::Mat& image) {
    put_frame(detail::main_frame_id, image, cv::Rect2d(0, 0, image.cols, image.rows), 0, true, true);
}

void Viewer::set_frame(FrameId id, const cv::Mat& image, const cv::Rect2d& world_rect, int layer) {
    put_frame(id, image.clone(), world_rect, layer, false, false);
}

void Viewer::set_frame_shared(FrameId id, const cv::Mat& image, const cv::Rect2d& world_rect, int layer) {
    put_frame(id, image, world_rect, layer, false, true);
}

void Viewer::remove_frame(FrameId id) {
    detail::frame_entry removed;
    {
        std::lock_guard<std::mutex> lock(m_state->frames_mutex);
        auto it = m_state->frames.find(id);
        if (it == m_state->frames.end()) return;
        removed = std::move(it->second);
        m_state->frames.erase(it);
        ++m_state->frames_version;
        m_state->latest_submit_time = detail::clock_type::now();
    }
    removed.image.release();
    removed.cache.reset();
    m_state->request_repaint();
}

void Viewer::move_frame(FrameId id, const cv::Rect2d& world_rect, std::optional<int> layer) {
    {
        std::lock_guard<std::mutex> lock(m_state->frames_mutex);
        auto it = m_state->frames.find(id);
        if (it == m_state->frames.end()) return;
        // Image, cache and live status are untouched, and frames_version isn't bumped: a move
        // isn't new content, so it doesn't show up in the "shown" / latency readout.
        it->second.world_rect = world_rect;
        if (layer) it->second.layer = *layer;
    }
    m_state->request_repaint();
}

void Viewer::clear_frames() {
    std::map<FrameId, detail::frame_entry> removed;
    {
        std::lock_guard<std::mutex> lock(m_state->frames_mutex);
        removed.swap(m_state->frames);
        ++m_state->frames_version;
        m_state->latest_submit_time = detail::clock_type::now();
    }
    removed.clear();
    m_state->request_repaint();
}

// ---- Waiting and events --------------------------------------------------------------------

Key Viewer::wait_key(int timeout_ms) {
    ViewerState& s = *m_state;
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));

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

            detail::queued_event ev = std::move(s.events.front());
            s.events.pop_front();

            if (ev.type == detail::queued_event::kind::mouse) {
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
            if (clock::now() >= deadline) return {};
            s.events_cv.wait_until(lock, deadline);
        } else {
            s.events_cv.wait(lock);
        }
    }
}

void Viewer::wait_until_closed() {
    while (is_open()) wait_key(-1);
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
    ui_thread::instance().post([s] {
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
    m_state->request_repaint(); // the help panel may be open
}

void Viewer::bind(std::string_view key_text, std::string description, std::function<void()> action) {
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

void Viewer::set_overlay_text(const std::string& id, std::string text, Qt::Corner corner) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        auto [it, inserted] = m_state->overlays.try_emplace(id);
        if (inserted) it->second.order = m_state->next_overlay_order++;
        it->second.text = std::move(text);
        it->second.corner = corner;
    }
    m_state->request_repaint();
}

void Viewer::set_overlay_rect(const std::string& id, const cv::Rect2d& world_rect, cv::Scalar colour,
                              int thickness_px, double fill_opacity) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        auto [it, inserted] = m_state->overlay_rects.try_emplace(id);
        if (inserted) it->second.order = m_state->next_overlay_order++;
        it->second.world_rect = world_rect;
        it->second.colour = colour;
        it->second.thickness_px = std::max(thickness_px, 0);
        it->second.fill_opacity = std::clamp(fill_opacity, 0.0, 1.0);
    }
    m_state->request_repaint();
}

void Viewer::remove_overlay(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->overlays.erase(id);
        m_state->overlay_rects.erase(id);
    }
    m_state->request_repaint();
}

void Viewer::show_fps(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->fps_enabled = enabled;
    }
    m_state->request_repaint();
}

// ---- Camera --------------------------------------------------------------------------------

Viewport Viewer::current_viewport() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->cam.as_viewport();
}

void Viewer::fit_all() {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->auto_fit = true;
    }
    m_state->request_repaint(); // the next paint fits and reports the new viewport
}

void Viewer::show_world_rect(const cv::Rect2d& world_rect) {
    bool queued;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        detail::fit_camera(m_state->cam, world_rect);
        m_state->auto_fit = false;
        queued = m_state->queue_viewport_locked();
    }
    if (queued) m_state->events_cv.notify_all();
    m_state->request_repaint();
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
    ui_thread::instance().post([s, t = QString::fromStdString(title)] {
        if (s->widget) s->widget->setWindowTitle(t);
    });
}

} // namespace fv
