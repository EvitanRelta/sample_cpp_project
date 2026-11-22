// demo_mosaic — the multi-resolution stitched-mosaic use case.
//
// A 4 x 3 grid of slightly overlapping "camera frames" (4096 x 4096 native pixels each) is
// stitched into one world. The app (this file), not the viewer, decides what to render:
//   - at startup it supplies every frame at 1/16 resolution (layer 0, the overview)
//   - when the user zooms in, it renders only the visible part of the visible frames at the
//     resolution the screen needs (layer 1, drawn over the blurry overview)
//   - detail tiles for frames that scroll out of view are removed
// Frame content is a Mandelbrot pattern, so there is genuine detail at every zoom level.
// Each frame is tinted slightly differently so you can see the stitched overlaps.
//
// Try: zoom in with the wheel and watch the blur sharpen; press D to turn detail off and
// compare; Ctrl+0 to fit; Ctrl+H for help.
#include <fv/viewer.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <thread>

// ---- Mosaic layout (all in world units = native pixels) ----------------------------------
static constexpr int GRID_COLS = 4, GRID_ROWS = 3;
static constexpr int FRAME_PX = 4096;   // native resolution of one frame
static constexpr int OVERLAP_PX = 256;  // neighbouring frames overlap by this much
static constexpr int STEP_PX = FRAME_PX - OVERLAP_PX;
static constexpr int OVERVIEW_LEVEL = 4;  // overview frames are 1/16 resolution (256 x 256)
static constexpr int FRAME_COUNT = GRID_COLS * GRID_ROWS;

// Forward declarations of helper functions
static cv::Rect2d frame_rect(int i);
static cv::Rect2d world_bounds();
static fv::FrameId overview_id(int i);
static fv::FrameId detail_id(int i);

namespace {

// ---- Frame "source": renders any world region of any frame at any resolution -------------
class MandelbrotSource {
   public:
    MandelbrotSource() {
        cv::Mat ramp(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
            ramp.at<uchar>(i) = static_cast<uchar>(i);
        cv::applyColorMap(ramp, m_palette, cv::COLORMAP_TURBO);
    }

    // Renders `region` (world coords, inside frame i) into an image of `size` pixels.
    cv::Mat render(int i, const cv::Rect2d& region, cv::Size size) const {
        cv::Mat out(size, CV_8UC3);
        const cv::Rect2d world = world_bounds();
        const double scale = m_plane_width / world.width;  // complex units per world unit
        const double cx0 = m_centre_re - m_plane_width / 2.0;
        const double cy0 = m_centre_im - world.height * scale / 2.0;
        const double px_w = region.width / size.width, px_h = region.height / size.height;

        // Slight per-frame tint so overlaps between frames are visible.
        const double tint_b = 0.80 + 0.20 * ((i * 7) % 5) / 4.0;
        const double tint_r = 0.80 + 0.20 * ((i * 3) % 4) / 3.0;

        cv::parallel_for_(cv::Range(0, size.height), [&](const cv::Range& rows) {
            for (int y = rows.start; y < rows.end; ++y) {
                auto* row = out.ptr<cv::Vec3b>(y);
                const double ci = cy0 + (region.y + (y + 0.5) * px_h) * scale;
                for (int x = 0; x < size.width; ++x) {
                    const double cr = cx0 + (region.x + (x + 0.5) * px_w) * scale;
                    double zr = 0, zi = 0;
                    int n = 0;
                    while (n < m_max_iter && zr * zr + zi * zi < 256.0) {
                        const double t = zr * zr - zi * zi + cr;
                        zi = 2 * zr * zi + ci;
                        zr = t;
                        ++n;
                    }
                    if (n == m_max_iter) {
                        row[x] = cv::Vec3b(0, 0, 0);
                        continue;
                    }
                    const double smooth = n + 1 - std::log2(std::log2(zr * zr + zi * zi) / 2.0);
                    const int idx = static_cast<int>(smooth * 6.0) & 255;
                    const cv::Vec3b c = m_palette.at<cv::Vec3b>(idx);
                    row[x] = cv::Vec3b(cv::saturate_cast<uchar>(c[0] * tint_b), c[1],
                                       cv::saturate_cast<uchar>(c[2] * tint_r));
                }
            }
        });
        return out;
    }

   private:
    cv::Mat m_palette;
    double m_centre_re = -0.65, m_centre_im = 0.0, m_plane_width = 3.4;
    int m_max_iter = 400;
};

// ---- Background worker: renders detail for the latest viewport only ----------------------
class DetailWorker {
   public:
    DetailWorker(fv::Viewer& viewer, const MandelbrotSource& source)
        : m_viewer(viewer), m_source(source), m_thread([this] { run(); }) {}

    ~DetailWorker() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }

    // Called on the viewer's owner thread. Newer requests replace older ones.
    void request(const fv::Viewport& v) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending = v;
            ++m_generation;
        }
        m_cv.notify_all();
    }

    void set_enabled(bool enabled) {
        m_enabled = enabled;
        request(m_viewer.current_viewport());
    }
    bool enabled() const { return m_enabled; }

   private:
    void run() {
        for (;;) {
            fv::Viewport v;
            std::uint64_t gen;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || m_pending; });
                if (m_stop) return;
                v = *m_pending;
                m_pending.reset();
                gen = m_generation;
            }
            update_detail(v, gen);
        }
    }

    bool stale(std::uint64_t gen) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stop || gen != m_generation;
    }

    void update_detail(const fv::Viewport& v, std::uint64_t gen) {
        // Resolution level the screen needs: level L = 1 / 2^L of native resolution.
        int level = static_cast<int>(std::floor(std::log2(std::max(v.world_per_screen_px, 1e-9))));
        level = std::clamp(level, 0, OVERVIEW_LEVEL);

        if (!m_enabled || level >= OVERVIEW_LEVEL) {
            for (int i = 0; i < FRAME_COUNT; ++i)
                m_viewer.remove_frame(detail_id(i));
            status(level, 0, false);
            return;
        }

        const int div = 1 << level;
        int tiles = 0;
        status(level, tiles, true);
        for (int i = 0; i < FRAME_COUNT; ++i) {
            if (stale(gen)) return;  // the user moved on; a newer request is waiting

            const cv::Rect2d frame = frame_rect(i);
            cv::Rect2d visible = frame & v.world_rect;
            if (visible.width <= 0 || visible.height <= 0) {
                m_viewer.remove_frame(detail_id(i));  // out of view: unrender
                continue;
            }
            // Snap to the level's pixel grid, relative to the frame.
            const double x0 = frame.x + std::floor((visible.x - frame.x) / div) * div;
            const double y0 = frame.y + std::floor((visible.y - frame.y) / div) * div;
            const double x1 =
                frame.x + std::ceil((visible.x + visible.width - frame.x) / div) * div;
            const double y1 =
                frame.y + std::ceil((visible.y + visible.height - frame.y) / div) * div;
            const cv::Rect2d region(x0, y0, x1 - x0, y1 - y0);
            const cv::Size size(static_cast<int>(region.width / div),
                                static_cast<int>(region.height / div));

            cv::Mat tile = m_source.render(i, region, size);
            m_viewer.set_frame(detail_id(i), tile, region, /*layer=*/1);
            ++tiles;
        }
        status(level, tiles, false);
    }

    void status(int level, int tiles, bool busy) {
        m_viewer.set_overlay_text("status",
                                  cv::format("detail: %s   level %d (1/%d res)   detail tiles: "
                                             "%d%s\nD: toggle detail   Ctrl+H: help",
                                             m_enabled ? "on" : "OFF", level, 1 << level, tiles,
                                             busy ? "   rendering..." : ""));
    }

    fv::Viewer& m_viewer;
    const MandelbrotSource& m_source;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<fv::Viewport> m_pending;
    std::uint64_t m_generation = 0;
    bool m_stop = false;
    std::atomic<bool> m_enabled{true};
    std::thread m_thread;  // last: starts after everything above is initialised
};

}  // namespace

int main() {
    fv::Viewer viewer("demo_mosaic", {1400, 900});
    MandelbrotSource source;

    // Overview: every frame at 1/16 resolution, on layer 0.
    const int overview_px = FRAME_PX >> OVERVIEW_LEVEL;
    for (int i = 0; i < FRAME_COUNT; ++i) {
        const cv::Rect2d rect = frame_rect(i);
        viewer.set_frame(overview_id(i), source.render(i, rect, {overview_px, overview_px}), rect,
                         0);
    }
    viewer.fit_all();

    DetailWorker worker(viewer, source);  // declared after viewer: stopped before the viewer closes

    viewer.on_viewport_changed([&](const fv::Viewport& v) { worker.request(v); });
    viewer.bind(Qt::Key_D, "Toggle detail tiles", [&] { worker.set_enabled(!worker.enabled()); });

    viewer.wait_until_closed();
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
static cv::Rect2d frame_rect(int i) {
    return {static_cast<double>((i % GRID_COLS) * STEP_PX),
            static_cast<double>((i / GRID_COLS) * STEP_PX), static_cast<double>(FRAME_PX),
            static_cast<double>(FRAME_PX)};
}

static cv::Rect2d world_bounds() {
    return {0, 0, static_cast<double>(STEP_PX * (GRID_COLS - 1) + FRAME_PX),
            static_cast<double>(STEP_PX * (GRID_ROWS - 1) + FRAME_PX)};
}

static fv::FrameId overview_id(int i) {
    return static_cast<fv::FrameId>(i);
}
static fv::FrameId detail_id(int i) {
    return static_cast<fv::FrameId>(1000 + i);
}
