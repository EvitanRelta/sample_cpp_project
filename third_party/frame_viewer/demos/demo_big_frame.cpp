// demo_big_frame — streaming very large frames without copying and without ever waiting.
//
// An "algorithm" renders full frames and hands them to the viewer with show_shared(), taking
// each buffer from a fv::BufferPool. The algorithm never waits for the display: if it
// produces frames faster than they can be drawn, the viewer simply draws the newest one.
//
//   ./demo_big_frame                      5K (5120 x 2880) at 15 FPS, no-copy mode
//   ./demo_big_frame --size 8k            7680 x 4320       (also: 4k, 5k, or WxH)
//   ./demo_big_frame --fps 0              as fast as possible (0 = unlimited)
//   ./demo_big_frame --copy               use show() (copies) instead, to compare
//
// The top-right readout shows the frame rate submitted, the rate actually drawn, and the
// latency from submit to drawn. Zoom in anywhere: full-resolution detail is there.
#include <fv/viewer.h>

#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

namespace {

// Fills the whole frame: a moving pattern plus a fine labelled grid to zoom into.
void render_into(cv::Mat& frame, int i) {
    const int shift = i * 12;
    cv::parallel_for_(cv::Range(0, frame.rows), [&](const cv::Range& rows) {
        for (int y = rows.start; y < rows.end; ++y) {
            auto* row = frame.ptr<cv::Vec3b>(y);
            for (int x = 0; x < frame.cols; ++x) {
                const bool grid = (x % 64 == 0) || (y % 64 == 0);
                row[x] = grid ? cv::Vec3b(200, 200, 200)
                              : cv::Vec3b(static_cast<uchar>((x + shift) >> 4), static_cast<uchar>((y + shift) >> 4),
                                          static_cast<uchar>(((x ^ y) >> 5) * 8));
            }
        }
    });
    for (int y = 512; y < frame.rows; y += 1024)
        for (int x = 256; x < frame.cols; x += 1024)
            cv::putText(frame, cv::format("(%d, %d) frame %d", x, y, i), {x, y}, cv::FONT_HERSHEY_SIMPLEX, 1.2,
                        {255, 255, 255}, 2, cv::LINE_AA);
}

cv::Size parse_size(const std::string& s) {
    if (s == "4k") return {3840, 2160};
    if (s == "5k") return {5120, 2880};
    if (s == "8k") return {7680, 4320};
    int w = 0, h = 0;
    if (std::sscanf(s.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) return {w, h};
    std::fprintf(stderr, "bad --size '%s' (use 4k, 5k, 8k or WxH)\n", s.c_str());
    std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
    cv::Size size(5120, 2880);
    double fps = 15.0;
    bool copy_mode = false;
    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--copy")) copy_mode = true;
        else if (!std::strcmp(argv[a], "--size") && a + 1 < argc) size = parse_size(argv[++a]);
        else if (!std::strcmp(argv[a], "--fps") && a + 1 < argc) fps = std::atof(argv[++a]);
        else {
            std::fprintf(stderr, "usage: %s [--size 4k|5k|8k|WxH] [--fps N] [--copy]\n", argv[0]);
            return 1;
        }
    }

    fv::Viewer viewer("demo_big_frame", {1280, 800});
    viewer.show_fps();

    fv::BufferPool pool(size, CV_8UC3); // 3 buffers, allocated up front
    cv::Mat copy_buffer(size, CV_8UC3);  // --copy mode reuses one buffer; show() copies it

    using clock = std::chrono::steady_clock;
    const auto period = fps > 0 ? std::chrono::duration<double>(1.0 / fps) : std::chrono::duration<double>(0);
    auto next_frame = clock::now();
    std::deque<double> show_times; // last second's call durations, for avg / max

    for (int i = 0; viewer.is_open(); ++i) {
        cv::Mat buf = copy_mode ? copy_buffer : pool.acquire();

        const auto t0 = clock::now();
        render_into(buf, i);
        const auto t1 = clock::now();
        if (copy_mode)
            viewer.show(buf);
        else
            viewer.show_shared(buf);
        const auto t2 = clock::now();

        const double render_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        show_times.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        const auto window = static_cast<std::size_t>(std::max(1.0, fps > 0 ? fps : 30.0));
        while (show_times.size() > window) show_times.pop_front();
        double sum = 0, worst = 0;
        for (double t : show_times) { sum += t; worst = std::max(worst, t); }

        viewer.set_overlay_text(
            "info",
            cv::format("%d x %d BGR (%.0f MB per frame)   target %s\n"
                       "mode: %s\n"
                       "render: %.1f ms   %s call: avg %.3f ms, max %.3f ms\n"
                       "pool buffers: %s",
                       size.width, size.height, buf.total() * buf.elemSize() / 1e6,
                       fps > 0 ? cv::format("%.0f FPS", fps).c_str() : "unlimited",
                       copy_mode ? "show() - copies" : "show_shared() - no copy",
                       render_ms, copy_mode ? "show()" : "show_shared()", sum / show_times.size(), worst,
                       copy_mode ? "n/a" : std::to_string(pool.buffer_count()).c_str()));
        buf.release(); // done with it; the pool can hand it out again once the viewer lets go

        // Pace to the target rate. wait_key() doubles as the sleep, so the window stays responsive.
        if (fps > 0) {
            next_frame += std::chrono::duration_cast<clock::duration>(period);
            const auto now = clock::now();
            if (next_frame < now) next_frame = now; // fell behind: don't try to catch up
            viewer.wait_key(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now).count()));
        } else {
            viewer.wait_key(0);
        }
    }
}
