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
// batch() applies several frame / overlay / camera changes as one update.
//
// Coordinates
// -----------
// Frames are placed in a shared "world" space (doubles). show() places its image at
// (0, 0, cols, rows), so in the simple case world coordinates are image pixel coordinates.
// Frame-attached fv::Box rects are image pixel coordinates, which for show() are the same as world
// coordinates; set_overlay_box() rects are world coordinates.
// set_frame() can rotate a frame about its rect centre; overlays and mouse positions stay in world
// coordinates either way.
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

#include <QtCore/qnamespace.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fv {

/** Caller-chosen id of a frame placed with set_frame(). The value ~0 is used by show(). */
using FrameId = std::uint64_t;

/** A box with a screen-space outline, optional fill and optional label. */
struct Box {
    cv::Rect2d rect;  // image pixels for frame-attached boxes (same as world coordinates for
                      // show()), world coordinates for set_overlay_box()
    cv::Scalar colour{0, 255, 0};  // BGR
    int thickness_px = 2;          // screen pixels; non-positive hides the outline
    std::string label;  // optional single-line text on a box-coloured tag, at a fixed screen size
    double fill_opacity = 0.0;  // in [0, 1]: fills the inside with `colour` (0 = outline only)
};

/** Visible region of a viewer. */
struct Viewport {
    cv::Rect2d world_rect;             // visible region in world coordinates
    double world_per_screen_px = 1.0;  // world units covered by one screen pixel (> 1 = zoomed out)
    cv::Size screen_size;              // window client area in pixels
};

/** Drawing statistics over the last second, from Viewer::stats(). Times in ms, rates per second. */
struct ViewerStats {
    double submitted_per_s = 0;  // frame submissions: show*(), set_frame*()
    double drawn_per_s = 0;      // paints that showed new frame content
    double replaced_per_s = 0;   // submissions replaced by a newer one for the same frame id
                                 // before being painted (newest wins)
    double paint_ms_avg = 0;     // whole paint, every paint (including overlay-only ones)
    double paint_ms_max = 0;
    double latency_ms_avg = 0;  // submission -> end of the first paint showing it
    double latency_ms_max = 0;
};

enum class MouseAction : std::uint8_t { PRESS, RELEASE, DOUBLE_CLICK, MOVE };

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
    Qt::MouseButton button = Qt::NoButton;  // the button that was pressed / released
    Qt::MouseButtons buttons;               // all buttons held after this event
    Qt::KeyboardModifiers modifiers;
    cv::Point2d screen_pos;  // window pixels
    cv::Point2d world_pos;   // world coordinates under the cursor
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
    Key(Qt::KeyboardModifiers modifiers, Qt::Key code)
        : m_combination(static_cast<int>(modifiers) | code) {}

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

namespace detail {
class ViewerState;
}

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
     * Any boxes attached to the previous image are removed.
     */
    void show(const cv::Mat& image);
    /**
     * Like show(image), plus boxes attached to the image and submitted with it as one update
     * (the image is copied, the boxes are not drawn into it). Every paint uses matching image
     * and box versions, even if newer submissions arrive while painting. The box list replaces
     * the previous list; pass std::move(boxes) to transfer it without copying.
     *
     * Boxes draw in list order above all frames and below persistent overlays, clipped to the
     * image. Labels stay the same screen size, above the box when space permits; long labels
     * are elided to fit the visible image. Neither boxes nor labels affect fit_all().
     */
    void show(const cv::Mat& image, std::vector<Box> boxes);

    /**
     * Like show(), but never copies or waits. The viewer holds a cv::Mat reference to the buffer
     * while it is shown or being drawn, so don't write into it meanwhile: take each new buffer
     * from a fv::BufferPool. The replaced buffer is released within this call (or when the
     * enclosing batch() ends), or after the paint drawing it.
     */
    void show_shared(const cv::Mat& image);
    /** Share the image with boxes as one update; ownership rules match show_shared(image). */
    void show_shared(const cv::Mat& image, std::vector<Box> boxes);

    /**
     * Add a frame in world coordinates, or replace the frame with this id. The image is copied.
     *
     * @param world_rect Placement before rotation; the image stretches to fit it
     * @param layer Higher layers draw on top; within a layer, frames draw in the order they
     *              were first added
     * @param angle_deg Rotation about the centre of world_rect, clockwise on screen (like
     *                  cv::RotatedRect). Frames below stay visible around a rotated frame. To map
     *                  a world point to image pixels, rotate it by -angle_deg about the centre,
     *                  then scale from world_rect to the image size.
     */
    void set_frame(FrameId id,
                   const cv::Mat& image,
                   const cv::Rect2d& world_rect,
                   int layer = 0,
                   double angle_deg = 0.0);
    /** Like set_frame(), but holds a reference instead of copying (same rules as show_shared()). */
    void set_frame_shared(FrameId id,
                          const cv::Mat& image,
                          const cv::Rect2d& world_rect,
                          int layer = 0,
                          double angle_deg = 0.0);
    /** Remove a frame; unknown ids are ignored. */
    void remove_frame(FrameId id);
    /** Remove all frames, including the show() image. */
    void clear_frames();

    /**
     * Move a frame without resubmitting its image. Keeps its cached downscaled copies and doesn't
     * count as new content, so dragging a frame around stays full quality.
     *
     * @param id Frame to move; unknown ids are ignored
     * @param world_rect New placement before rotation; the image stretches to fit it
     * @param layer New layer, or empty to keep the current one
     * @param angle_deg New rotation (see set_frame()), or empty to keep the current one
     */
    void move_frame(FrameId id,
                    const cv::Rect2d& world_rect,
                    std::optional<int> layer = {},
                    std::optional<double> angle_deg = {});

    /**
     * Run `changes` and apply every change it makes to this viewer at once: no paint shows only
     * some of them, and together they cause a single repaint. Frame, overlay (text, box, polygon)
     * and camera (fit_all(), show_world_rect()) calls made on this thread are staged, then applied
     * in order when the outermost batch ends. Nested batches join the outer one.
     *
     * - show() / set_frame() still copy the image when called, so you can reuse your buffer right
     *   away. *_shared() still never copies or waits; the viewer holds the buffer from the call,
     *   and one replaced within the batch is released when the batch ends.
     * - Getters (current_viewport(), screen_to_world(), world_to_screen(), stats()) return the
     *   state from before the batch until it ends.
     * - Other calls (bind(), show_fps(), set_title(), close(), ...) apply immediately.
     * - Calls from other threads apply on their own, never in the middle of this batch.
     * - If `changes` throws, nothing it staged is applied and the exception propagates.
     * - Submissions count in stats() when the batch ends.
     * - wait_key() / wait_until_closed() may not be called inside; they throw std::logic_error.
     *
     *     viewer.batch([&] {
     *         viewer.set_frame_shared(0, image, world_rect);
     *         viewer.set_overlay_box("target_7", box);
     *     });
     */
    void batch(const std::function<void()>& changes);

    // ---- Waiting and events (call from the owner thread) --------------------------------

    /**
     * Wait for a key press in this window, running this viewer's callbacks on the calling thread
     * meanwhile. Keys bound with bind() run their action and are not returned.
     *
     * @param timeout_ms Maximum wait; 0 only processes pending events, < 0 waits until a key
     *                   press or close
     *
     * @return The key, or an empty key on timeout or once the window is closed
     *
     * @throws std::logic_error if called inside batch() on this viewer
     */
    Key wait_key(int timeout_ms = 1);

    /** Run this viewer's callbacks until the window is closed. Throws like wait_key(). */
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

    /**
     * Show text in a window corner, unaffected by zoom and pan. Reusing an id replaces it.
     * Texts in a corner share one translucent box, in the order their ids were first set; each
     * keeps its own colour, so give a line its own id to colour it alone.
     *
     * @param colour BGR, like Box::colour; the default is the light grey of the other texts
     */
    void set_overlay_text(const std::string& id,
                          std::string text,
                          Qt::Corner corner = Qt::TopLeftCorner,
                          cv::Scalar colour = {235, 235, 235});
    /**
     * Draw a box above all frames (including frame-attached boxes) and below text, in world
     * coordinates, so it follows zoom and pan (pixel (x, y) covers world [x, x+1)). Reusing an id
     * moves or restyles it, which only repaints. Boxes don't affect fit_all().
     *
     * The outline width and label keep their screen size at any zoom. The label is drawn like a
     * frame-attached box's label, but clipped to the window instead of an image.
     *
     *     viewer.set_overlay_box("target_7", {world_rect, colour, 2, "7"});
     */
    void set_overlay_box(const std::string& id, const Box& box);
    /** Draw an unlabelled box; forwards to set_overlay_box(). */
    [[deprecated("Use set_overlay_box() instead")]]
    void set_overlay_rect(const std::string& id,
                          const cv::Rect2d& world_rect,
                          cv::Scalar colour,
                          int thickness_px = 2,
                          double fill_opacity = 0.0);
    /**
     * Draw a polygon or an open polyline, like set_overlay_box() (lines keep their screen width,
     * reusing an id only repaints). Polygons draw above boxes and don't affect fit_all().
     *
     * @param world_pts Vertices in world coordinates; a single vertex draws a dot
     * @param closed Join the last vertex back to the first; only closed polygons are filled
     */
    void set_overlay_polygon(const std::string& id,
                             std::vector<cv::Point2d> world_pts,
                             cv::Scalar colour,
                             int thickness_px = 2,
                             double fill_opacity = 0.0,
                             bool closed = true);
    /** Remove the overlay text, box and polygon with this id. */
    void remove_overlay(const std::string& id);
    /**
     * Show or hide a top-right readout of stats(): submitted / drawn / replaced rates, and paint
     * time and latency as avg/max.
     */
    void show_fps(bool enabled = true);
    /** Return the drawing statistics over the last second. Works with or without show_fps(). */
    ViewerStats stats() const;

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
    void put_frame(FrameId id,
                   cv::Mat image,
                   const cv::Rect2d& world_rect,
                   int layer,
                   double angle_deg,
                   bool shared,
                   std::vector<Box> boxes = {});

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

}  // namespace fv
