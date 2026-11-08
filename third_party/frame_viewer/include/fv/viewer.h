/** Public API of the frame viewer: fv::Viewer (an OpenCV image window) and fv::BufferPool. */
//
// Threading model
// ---------------
// The library runs its own hidden UI thread (started on first use, stopped at program exit), so
// you never create a Qt application yourself. Every fv::Viewer method is safe from any thread.
//
// The thread that calls wait_key() / wait_until_closed() on a viewer is its "owner thread". All
// callbacks registered on a viewer (key bindings, mouse, viewport) run on that thread, inside
// those calls. So callbacks never race with your loop, and never run once you stop waiting.
//
// Coordinates
// -----------
// Frames are placed in a shared "world" space (doubles). show() places its image at
// (0, 0, cols, rows), so in the simple case world coordinates are image pixel coordinates.
//
// Built-in controls (listed in the Ctrl+H help)
// ---------------------------------------------
//   Mouse wheel           zoom at cursor
//   Ctrl + left drag      pan
//   Ctrl + '+' or '='     zoom in
//   Ctrl + '-'            zoom out
//   Ctrl + 0              fit all content
//   Ctrl + H              toggle help
//   Esc twice (quickly)   close window
// Binding one of these keys with bind() overrides it.
#pragma once

#include <opencv2/core.hpp>

#include <QtCore/qnamespace.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fv {

/** Caller-chosen id of a frame placed with set_frame(). The value ~0 is used by show(). */
using FrameId = std::uint64_t;

/** Visible region of a viewer. */
struct Viewport {
    cv::Rect2d world_rect;            // visible region in world coordinates
    double world_per_screen_px = 1.0; // world units covered by one screen pixel (> 1 = zoomed out)
    cv::Size screen_size;             // window client area in pixels
};

enum class MouseAction { PRESS, RELEASE, DOUBLE_CLICK, MOVE };

/**
 * A mouse press, release, double-click or drag step, delivered to on_mouse(). Ctrl + left
 * button is reserved for panning and is not reported.
 *
 * A drag is press, move..., release. `move` is only reported while a button is held (no hover),
 * with `button` = Qt::NoButton. Moves are merged if you fall behind, so you get the latest
 * position rather than a backlog.
 */
struct MouseEvent {
    MouseAction action = MouseAction::PRESS;
    Qt::MouseButton button = Qt::NoButton; // the button that was pressed / released
    Qt::MouseButtons buttons;              // all buttons held after this event
    Qt::KeyboardModifiers modifiers;
    cv::Point2d screen_pos; // window pixels
    cv::Point2d world_pos;  // world coordinates under the cursor
};

/**
 * A key with modifiers, e.g. returned by wait_key() (empty on timeout or close). Converts
 * implicitly from Qt::Key and from Qt 5 key combinations (modifiers OR'd with a key):
 *
 *     fv::Key k = viewer.wait_key();
 *     if (k == Qt::Key_Space) ...
 *     if (k == (Qt::CTRL | Qt::Key_S)) ...
 *     if (k) ...  // any key pressed?
 */
class Key {
public:
    Key() = default;
    /** @param combination A Qt::Key, optionally OR'd with modifiers, e.g. Qt::CTRL | Qt::Key_S */
    Key(int combination) : m_combination(combination) {}
    /** Accept Qt::ControlModifier | Qt::Key_S, which Qt 5 types as QIncompatibleFlag. */
    Key(QIncompatibleFlag combination) : m_combination(static_cast<int>(combination)) {}
    Key(Qt::KeyboardModifiers modifiers, Qt::Key code) : m_combination(static_cast<int>(modifiers) | code) {}

    Qt::Key code() const { return static_cast<Qt::Key>(m_combination & ~Qt::KeyboardModifierMask); }
    Qt::KeyboardModifiers modifiers() const {
        return Qt::KeyboardModifiers(QFlag(m_combination & Qt::KeyboardModifierMask));
    }
    /** Return the key and modifiers as one int, as taken by QKeySequence(int). */
    int combination() const { return m_combination; }
    explicit operator bool() const { return code() != 0 && code() != Qt::Key_unknown; }

    friend bool operator==(Key a, Key b) { return a.m_combination == b.m_combination; }
    friend bool operator!=(Key a, Key b) { return !(a == b); }

private:
    int m_combination = 0;
};

namespace detail { class ViewerState; }

/** A window that displays OpenCV images, with zoom, pan, overlays and input callbacks. */
class Viewer {
public:
    /** Open a window. It appears asynchronously. */
    explicit Viewer(std::string title = "viewer", cv::Size size = {1280, 800});
    /** Close the window. */
    ~Viewer();

    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;

    // ---- Frames -------------------------------------------------------------------------

    /**
     * Display an image at world rect (0, 0, cols, rows), replacing the previous show() image,
     * like cv::imshow(). The image is copied, so reusing your buffer afterwards is safe.
     */
    void show(const cv::Mat& image);

    /**
     * Like show(), but never copies or waits. The viewer holds a cv::Mat reference to the buffer
     * while it is shown or being drawn, so don't write into it meanwhile: take each new buffer
     * from a fv::BufferPool. The replaced buffer is released within this call, or after the
     * paint drawing it.
     */
    void show_shared(const cv::Mat& image);

    /**
     * Add a frame in world coordinates, or replace the frame with this id. The image is copied.
     *
     * @param world_rect Placement; the image stretches to fit it
     * @param layer Higher layers draw on top; within a layer, frames draw in the order they
     *              were first added
     */
    void set_frame(FrameId id, const cv::Mat& image, const cv::Rect2d& world_rect, int layer = 0);
    /** Like set_frame(), but holds a reference instead of copying (same rules as show_shared()). */
    void set_frame_shared(FrameId id, const cv::Mat& image, const cv::Rect2d& world_rect, int layer = 0);
    /** Remove a frame; unknown ids are ignored. */
    void remove_frame(FrameId id);
    /** Remove all frames, including the show() image. */
    void clear_frames();

    /**
     * Move a frame without resubmitting its image. Keeps its cached downscaled copies and doesn't
     * count as new content, so dragging a frame around stays full quality.
     *
     * @param id Frame to move; unknown ids are ignored
     * @param world_rect New placement; the image stretches to fit it
     * @param layer New layer, or empty to keep the current one
     */
    void move_frame(FrameId id, const cv::Rect2d& world_rect, std::optional<int> layer = {});

    // ---- Waiting and events (call from the owner thread) --------------------------------

    /**
     * Wait for a key press in this window, running this viewer's callbacks on the calling thread
     * meanwhile. Keys bound with bind() run their action and are not returned.
     *
     * @param timeout_ms Maximum wait; 0 only processes pending events, < 0 waits until a key
     *                   press or close
     *
     * @return The key, or an empty key on timeout or once the window is closed
     */
    Key wait_key(int timeout_ms = 1);

    /** Run this viewer's callbacks until the window is closed. */
    void wait_until_closed();

    /** Return false once the window is closed (by the user, Esc twice, or close()). */
    bool is_open() const;
    /** Close the window, waking any wait_key() / wait_until_closed(). */
    void close();

    // ---- Callbacks (run on the owner thread, inside wait_key / wait_until_closed) -------

    /**
     * Bind a key to an action, replacing any earlier binding or built-in control for that key.
     * Shift is ignored on symbol keys, so Ctrl + '+' matches however '+' is typed.
     *
     * @param description Shown next to the key in the Ctrl+H help
     */
    void bind(Key key, std::string description, std::function<void()> action);
    /**
     * Bind a key given as text, e.g. "Ctrl+S". See the other overload.
     *
     * @throws std::invalid_argument if `key_text` isn't a single key combination
     */
    void bind(std::string_view key_text, std::string description, std::function<void()> action);
    /** Set the mouse callback, replacing any previous one. See MouseEvent. */
    void on_mouse(std::function<void(const MouseEvent&)> callback);
    /** Set the callback for when the visible region changes. Coalesced: you get the latest. */
    void on_viewport_changed(std::function<void(const Viewport&)> callback);

    // ---- Overlays -----------------------------------------------------------------------

    /** Show text in a window corner, unaffected by zoom and pan. Reusing an id replaces it. */
    void set_overlay_text(const std::string& id, std::string text, Qt::Corner corner = Qt::TopLeftCorner);
    /**
     * Draw a box above all frames and below text. Reusing an id moves or restyles it, which only
     * repaints. Boxes don't affect fit_all().
     *
     * @param world_rect Box in world coordinates, so it follows zoom and pan (pixel (x, y)
     *                   covers world [x, x+1))
     * @param colour BGR
     * @param thickness_px Line width in screen pixels, the same at any zoom
     * @param fill_opacity In [0, 1]: fills the inside with `colour` (0 = outline only)
     */
    void set_overlay_rect(const std::string& id, const cv::Rect2d& world_rect, cv::Scalar colour,
                          int thickness_px = 2, double fill_opacity = 0.0);
    /** Remove the overlay text and box with this id. */
    void remove_overlay(const std::string& id);
    /**
     * Show or hide a top-right readout: "FPS" = rate of show() / show_shared() calls, "shown" =
     * rate of new content actually drawn (lower when frames are dropped), "latency" = submit to
     * drawn.
     */
    void show_fps(bool enabled = true);

    // ---- Camera -------------------------------------------------------------------------

    Viewport current_viewport() const;
    /**
     * Zoom to fit all frames, and keep fitting as content changes until the camera is moved.
     * New viewers start in this mode.
     */
    void fit_all();
    /** Zoom and pan to fit this world rect in the window. Stops fit_all(). */
    void show_world_rect(const cv::Rect2d& world_rect);
    cv::Point2d screen_to_world(cv::Point2d screen_pos) const;
    cv::Point2d world_to_screen(cv::Point2d world_pos) const;

    void set_title(const std::string& title);

private:
    void put_frame(FrameId id, cv::Mat image, const cv::Rect2d& world_rect, int layer,
                   bool counts_for_fps, bool shared);

    std::shared_ptr<detail::ViewerState> m_state;
};

/**
 * Preallocated buffers for show_shared() / set_frame_shared().
 *
 * acquire() only returns buffers nothing else references (not the viewer, not a paint in
 * progress, not a buffer you still hold), so you can always write into them and neither you nor
 * the viewer ever waits for the other. The viewer holds at most two buffers (the one shown and
 * one being drawn), so with the default of 3, acquire() normally never allocates or waits.
 *
 *     fv::BufferPool pool({5120, 2880}, CV_8UC3);
 *     while (viewer.is_open()) {
 *         cv::Mat buf = pool.acquire();
 *         render_into(buf);
 *         viewer.show_shared(buf);
 *         viewer.wait_key();
 *     }
 *
 * Extra buffers don't add latency: the viewer always draws the newest frame and drops older
 * ones that were never drawn.
 */
class BufferPool {
public:
    /**
     * Allocate `initial_count` buffers of `size` and `type` (e.g. CV_8UC3).
     *
     * @param max_count Cap on buffers; once all are allocated and busy, acquire() waits
     */
    BufferPool(cv::Size size, int type, int initial_count = 3, int max_count = 8);

    /**
     * Return a buffer nothing else references. If all are busy, allocate another (up to
     * max_count) or wait for one to free up. Thread-safe.
     */
    cv::Mat acquire();
    /** Return the number of buffers allocated so far. */
    int buffer_count() const;

    /** Return true if only the pool references this buffer, so nobody can be reading it. */
    static bool is_free(const cv::Mat& buffer);

private:
    mutable std::mutex m_mutex;
    std::vector<cv::Mat> m_buffers;
    cv::Size m_size;
    int m_type;
    int m_max_count;
    std::size_t m_next = 0;
};

} // namespace fv
