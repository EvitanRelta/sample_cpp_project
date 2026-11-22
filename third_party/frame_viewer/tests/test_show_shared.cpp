// test_show_shared — checks that show_shared() doesn't copy and never waits, and that
// fv::BufferPool never hands out a buffer the viewer is still using.
//
//   ./build/bin/test_show_shared                 5K frames (default)
//   ./build/bin/test_show_shared --size 8k       also: 4k, 5k, WxH
//   ./build/bin/test_show_shared --seconds 10    length of the streaming test (default 5)
//
// Opens a window for the duration of the test and closes it at the end. Prints a table of
// measured vs expected values; the exit code is 0 if every check passes.
//
// Timings are reported two ways: "cpu" is the CPU time the call itself used on the calling
// thread (the work it does, e.g. copying), "wall" is elapsed time, which also includes any time
// the OS gave to other threads in between. On machines with few cores, the display thread can
// run in the middle of a call, so wall time is only checked with 2+ cores.
#include <fv/viewer.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

using ClockType = std::chrono::steady_clock;

// Forward declarations of helper functions
static int refcount(const cv::Mat& m);
static double rss_mb();
static double ms_since(ClockType::time_point t);
static double thread_cpu_ms();
static void settle(fv::Viewer& v, int ms);
static cv::Mat fresh_frame(cv::Size size, int value);
static void row(const char* check, const std::string& measured, const char* expected, bool pass);
static void info(const char* check, const std::string& measured, const char* note);
static void header(const char* title);

namespace {

// Measures one call: wall and calling-thread CPU time in ms.
struct Timing {
    double wall = 0, cpu = 0;
};

template <class F>
Timing measure(F&& f) {
    const auto w = ClockType::now();
    const double c = thread_cpu_ms();
    f();
    return {ms_since(w), thread_cpu_ms() - c};
}

}  // namespace

static int failures = 0;

int main(int argc, char** argv) {
    cv::Size size(5120, 2880);
    double seconds = 5;
    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--size") && a + 1 < argc) {
            const std::string s = argv[++a];
            if (s == "4k")
                size = {3840, 2160};
            else if (s == "5k")
                size = {5120, 2880};
            else if (s == "8k")
                size = {7680, 4320};
            else if (std::sscanf(s.c_str(), "%dx%d", &size.width, &size.height) != 2)
                return 1;
        } else if (!std::strcmp(argv[a], "--seconds") && a + 1 < argc) {
            seconds = std::atof(argv[++a]);
        } else {
            std::fprintf(stderr, "usage: %s [--size 4k|5k|8k|WxH] [--seconds N]\n", argv[0]);
            return 1;
        }
    }
    const double frame_mb = size.area() * 3 / (1024.0 * 1024.0);
    const unsigned cores = std::thread::hardware_concurrency();
    std::printf("test_show_shared: %d x %d BGR frames (%.1f MB each), %u CPU cores\n", size.width,
                size.height, frame_mb, cores);

    fv::Viewer v("test_show_shared", {1280, 800});
    v.show_fps();
    settle(v, 500);  // let the window appear

    // ---- 1. Reference counts: does the viewer share the buffer or copy it? ---------------
    header("1. No copy (cv::Mat reference counts)");
    {
        cv::Mat a = fresh_frame(size, 60), b = fresh_frame(size, 120), c = fresh_frame(size, 180);
        const int before = refcount(a);
        v.show_shared(a);
        settle(v, 300);
        const int held = refcount(a);
        row("show_shared(A): A's count before -> after",
            std::to_string(before) + " -> " + std::to_string(held), "1 -> 2 (viewer shares A)",
            before == 1 && held == 2);

        v.show_shared(b);
        settle(v, 300);
        row("after show_shared(B): A's count", std::to_string(refcount(a)), "1 (viewer released A)",
            refcount(a) == 1);

        v.show(c);
        settle(v, 300);
        row("show(C): C's count", std::to_string(refcount(c)), "1 (show() copied C)",
            refcount(c) == 1);
        v.clear_frames();
        settle(v, 300);
    }

    // ---- 2. Memory: does showing a frame allocate another frame's worth? ----------------
    header("2. Memory (process RSS growth when showing a new frame)");
    {
        cv::Mat d = fresh_frame(size, 90), e = fresh_frame(size, 150);
        settle(v, 200);
        double base = rss_mb();
        v.show_shared(d);
        settle(v, 400);
        const double shared_growth = rss_mb() - base;
        row("show_shared(D): RSS growth", cv::format("%.1f MB", shared_growth),
            cv::format("< %.0f MB (small cache)", frame_mb * 0.2).c_str(),
            shared_growth < frame_mb * 0.2);

        base = rss_mb();
        v.show(e);
        settle(v, 400);
        const double copy_growth = rss_mb() - base;
        row("show(E): RSS growth", cv::format("%.1f MB", copy_growth),
            cv::format("> %.0f MB (one copy)", frame_mb * 0.8).c_str(),
            copy_growth > frame_mb * 0.8);
        v.clear_frames();
        settle(v, 300);
    }

    // ---- 3. Call time with an idle viewer ---------------------------------------------
    header("3. Call time when no paint is in progress (average of 10)");
    {
        cv::Mat f = fresh_frame(size, 100);
        Timing shared{}, copied{}, clone{}, replace_copy{};
        for (int i = 0; i < 10; ++i) {
            // Replacing show()'s copy frees it inside the call (measured separately below), so
            // clear it first to time show_shared() on its own.
            v.clear_frames();
            settle(v, 150);
            const Timing a = measure([&] { v.show_shared(f); });
            settle(v, 150);
            const Timing b = measure([&] { v.show(f); });
            const Timing c =
                measure([&] { cv::Mat reference = f.clone(); });  // one plain copy, for comparison
            settle(v, 150);
            const Timing d = measure([&] { v.show_shared(f); });  // frees show()'s copy
            shared.wall += a.wall / 10;
            shared.cpu += a.cpu / 10;
            copied.wall += b.wall / 10;
            copied.cpu += b.cpu / 10;
            clone.wall += c.wall / 10;
            clone.cpu += c.cpu / 10;
            replace_copy.wall += d.wall / 10;
            replace_copy.cpu += d.cpu / 10;
        }
        row("show_shared(): cpu", cv::format("%.3f ms", shared.cpu), "< 0.5 ms (no copy)",
            shared.cpu < 0.5);
        if (cores >= 2)
            row("show_shared(): wall", cv::format("%.3f ms", shared.wall), "< 1 ms",
                shared.wall < 1.0);
        else
            info("show_shared(): wall", cv::format("%.3f ms", shared.wall),
                 "(1 core: not checked)");
        row("show(): cpu", cv::format("%.2f ms", copied.cpu),
            cv::format("~ clone(): %.2f ms", clone.cpu).c_str(), copied.cpu > 0.5 * clone.cpu);
        info("show_shared() replacing a show() copy: wall",
             cv::format("%.3f ms", replace_copy.wall), "(frees the copy)");
        v.clear_frames();
        settle(v, 300);
    }

    // ---- 4. Streaming with a BufferPool ----------------------------------------------
    header(cv::format("4. Streaming for %.0f s as fast as possible (BufferPool + show_shared)",
                      seconds)
               .c_str());
    {
        fv::BufferPool pool(size, CV_8UC3);
        int frames = 0, busy_on_acquire = 0;
        Timing total{}, worst{};
        const auto end = ClockType::now() + std::chrono::duration<double>(seconds);
        while (ClockType::now() < end && v.is_open()) {
            cv::Mat buf = pool.acquire();
            if (refcount(buf) != 2)
                ++busy_on_acquire;  // 2 = the pool's header + ours; 3+ = someone else
            buf.setTo(cv::Scalar::all((frames * 7) % 256));  // overwrite the whole buffer
            const Timing t = measure([&] { v.show_shared(buf); });
            total.wall += t.wall;
            total.cpu += t.cpu;
            worst.wall = std::max(worst.wall, t.wall);
            worst.cpu = std::max(worst.cpu, t.cpu);
            ++frames;
            buf.release();
            v.wait_key(0);  // run callbacks without waiting
        }
        const double n = std::max(frames, 1);
        info("frames submitted", std::to_string(frames), "(see 'shown' in the window)");
        row("show_shared(): cpu average", cv::format("%.4f ms", total.cpu / n), "< 0.1 ms",
            total.cpu / n < 0.1);
        row("show_shared(): cpu worst", cv::format("%.3f ms", worst.cpu), "< 1 ms",
            worst.cpu < 1.0);
        if (cores >= 2) {
            row("show_shared(): wall average", cv::format("%.4f ms", total.wall / n), "< 0.2 ms",
                total.wall / n < 0.2);
            row("show_shared(): wall worst", cv::format("%.3f ms", worst.wall),
                "< 5 ms (never waits)", worst.wall < 5);
        } else {
            info("show_shared(): wall average", cv::format("%.4f ms", total.wall / n),
                 "(1 core: not checked)");
            info("show_shared(): wall worst", cv::format("%.3f ms", worst.wall),
                 "(1 core: not checked)");
        }
        row("acquire() gave a buffer the viewer still held", std::to_string(busy_on_acquire), "0",
            busy_on_acquire == 0);
        row("buffers the pool needed", std::to_string(pool.buffer_count()), "3",
            pool.buffer_count() <= 3);
    }

    std::printf("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "SOME CHECKS FAILED",
                failures);
    v.close();
    return failures == 0 ? 0 : 1;
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
static int refcount(const cv::Mat& m) {
    return m.u ? __atomic_load_n(&m.u->refcount, __ATOMIC_ACQUIRE) : 0;
}

static double rss_mb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            double kb = 0;
            status >> kb;
            return kb / 1024.0;
        }
        std::getline(status, key);
    }
    return 0;
}

static double ms_since(ClockType::time_point t) {
    return std::chrono::duration<double, std::milli>(ClockType::now() - t).count();
}

static double thread_cpu_ms() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/** Let the viewer paint (and release whatever it should) while keeping the window responsive. */
static void settle(fv::Viewer& v, int ms) {
    const auto end = ClockType::now() + std::chrono::milliseconds(ms);
    while (ClockType::now() < end)
        v.wait_key(10);
}

static cv::Mat fresh_frame(cv::Size size, int value) {
    return cv::Mat(size, CV_8UC3, cv::Scalar::all(value));  // allocated and touched (counts in RSS)
}

static void row(const char* check, const std::string& measured, const char* expected, bool pass) {
    std::printf("  %-46s %-24s %-28s %s\n", check, measured.c_str(), expected,
                pass ? "PASS" : "FAIL");
    if (!pass) ++failures;
}

static void info(const char* check, const std::string& measured, const char* note) {
    std::printf("  %-46s %-24s %-28s %s\n", check, measured.c_str(), note, "info");
}

static void header(const char* title) {
    std::printf("\n%s\n  %-46s %-24s %-28s %s\n", title, "check", "measured", "expected", "result");
}
