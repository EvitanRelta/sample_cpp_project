// Run directly, with no display needed: QT_QPA_PLATFORM=offscreen ./build/bin/test_batch
#include <fv/viewer.h>
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QWidget>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "ui_thread.h"

// Forward declarations of helper functions
static QImage capture();
static void check(bool condition, const char* message);

static int failures = 0;

int main() {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_SCALE_FACTOR", "1");
    fv::Viewer viewer("test_batch", {320, 240});
    const cv::Rect2d world(0, 0, 320, 240);
    viewer.show_world_rect(world);

    // A worker submits a frame and its overlay box together, alternating between two versions:
    // a red image with a green box, and a blue image with a yellow box. Every paint must show a
    // matching pair. Image pixel (160, 120) is inside the box; (100, 70) is on its top edge.
    const QColor red(255, 0, 0), green(0, 255, 0), blue(0, 0, 255), yellow(255, 255, 0);
    const cv::Mat images[2] = {cv::Mat(240, 320, CV_8UC3, cv::Scalar(0, 0, 255)),
                               cv::Mat(240, 320, CV_8UC3, cv::Scalar(255, 0, 0))};
    const cv::Scalar box_colours[2] = {cv::Scalar(0, 255, 0), cv::Scalar(0, 255, 255)};
    const cv::Rect2d box_rect(60, 70, 120, 100);
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        for (int i = 0; !stop; ++i) {
            const int k = i % 2;
            viewer.batch([&] {
                viewer.set_frame_shared(0, images[k], world);
                viewer.set_overlay_box("box", {box_rect, box_colours[k], 2, ""});
            });
        }
    });
    int mismatches = 0, red_paints = 0, blue_paints = 0;
    for (int n = 0; n < 200; ++n) {
        const QImage shown = capture();
        const QColor image_px = shown.pixelColor(160, 120), box_px = shown.pixelColor(100, 70);
        if (image_px == red && box_px == green)
            ++red_paints;
        else if (image_px == blue && box_px == yellow)
            ++blue_paints;
        else
            ++mismatches;
    }
    stop = true;
    worker.join();
    std::printf("paints: %d red, %d blue, %d mismatched\n", red_paints, blue_paints, mismatches);
    check(mismatches == 0, "no paint shows a frame with the other batch's box");
    check(red_paints > 0 && blue_paints > 0, "paints saw both versions");

    viewer.remove_overlay("box");
    viewer.set_frame(0, images[1], world);  // a known (blue) frame to compare against
    // Let the stats window empty, so this batch's single submission is the only one counted.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    cv::Mat buffer(240, 320, CV_8UC3, cv::Scalar(0, 255, 0));
    const fv::Viewport before = viewer.current_viewport();
    viewer.batch([&] {
        viewer.set_frame(0, buffer, world);
        buffer.setTo(cv::Scalar(255, 0, 0));  // set_frame() already copied, so this is safe
        viewer.show_world_rect(cv::Rect2d(0, 0, 160, 120));
        check(viewer.current_viewport().world_rect == before.world_rect,
              "getters return the state from before the batch");
        check(viewer.stats().submitted_per_s == 0, "submissions count only when the batch ends");
        check(capture().pixelColor(160, 120) == blue, "paints don't show an unfinished batch");
        bool threw = false;
        try {
            viewer.wait_key(0);
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "wait_key() inside a batch throws std::logic_error");
    });
    check(viewer.stats().submitted_per_s > 0, "submissions count when the batch ends");
    check(viewer.current_viewport().world_rect == cv::Rect2d(0, 0, 160, 120),
          "batched camera change applies when the batch ends");
    check(capture().pixelColor(160, 120) == green, "set_frame() copied the image when called");

    // Nested batches join the outer one; a throwing batch applies nothing it staged.
    const cv::Mat grey(240, 320, CV_8UC3, cv::Scalar::all(128));
    viewer.batch([&] {
        viewer.batch([&] { viewer.set_frame(0, grey, world); });
        check(capture().pixelColor(160, 120) == green, "a nested batch waits for the outer one");
    });
    check(capture().pixelColor(160, 120) == QColor(128, 128, 128), "the outer batch applies");
    bool propagated = false;
    try {
        viewer.batch([&] {
            viewer.set_frame(0, images[0], world);
            throw std::runtime_error("changes failed");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    check(propagated, "an exception from changes propagates");
    check(capture().pixelColor(160, 120) == QColor(128, 128, 128),
          "a throwing batch applies none of its changes");
    viewer.batch([&] { viewer.set_frame(0, images[1], world); });
    check(capture().pixelColor(160, 120) == blue, "batches work again after a throwing one");

    viewer.close();
    std::printf("test_batch: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
/** Capture the viewer on its UI thread, after previously posted updates. */
static QImage capture() {
    QImage image;
    fv::detail::UiThread::instance().post_and_wait([&] {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (window->windowTitle() != QStringLiteral("test_batch")) continue;
            image = window->grab().toImage().convertToFormat(QImage::Format_RGB32);
            break;
        }
    });
    return image;
}

static void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}
