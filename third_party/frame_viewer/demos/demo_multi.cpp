// demo_multi — two threads, each running its own "algorithm" in its own window.
//
// main() is plain std::thread code: no viewer setup, no quit calls.
// Keys and clicks only go to the focused window:
//   "edges" window:  + / -   change the Canny threshold
//   "blobs" window:  Space   pause / resume (returned by wait_key, handled in the loop)
//                    click   print the clicked position
// Close a window (or press Esc twice in it) to end that thread; the program exits when both are done.
#include <fv/viewer.h>

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>
#include <thread>

namespace {

// A moving synthetic scene shared by both algorithms.
void make_scene(cv::Mat& out, int i) {
    out.create(600, 800, CV_8UC3);
    out.setTo(cv::Scalar(30, 30, 30));
    const double t = i * 0.04;
    cv::rectangle(out, cv::Rect(100 + static_cast<int>(80 * std::sin(t)), 100, 200, 150), {200, 180, 60}, -1);
    cv::circle(out, {500 + static_cast<int>(120 * std::cos(t * 0.8)), 300}, 90, {60, 160, 230}, -1, cv::LINE_AA);
    cv::ellipse(out, {300, 450 + static_cast<int>(40 * std::sin(t * 1.3))}, {140, 60}, t * 20, 0, 360,
                {120, 220, 120}, -1, cv::LINE_AA);
    cv::Mat noise(out.size(), CV_8UC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(12));
    out += noise;
}

void edge_detection() {
    fv::Viewer viewer("edges", {800, 600});
    viewer.show_fps();

    int threshold = 80; // plain int: bindings run on this thread, inside wait_key()
    viewer.bind(Qt::Key_Plus, "Raise Canny threshold", [&] { threshold = std::min(threshold + 10, 250); });
    viewer.bind(Qt::Key_Minus, "Lower Canny threshold", [&] { threshold = std::max(threshold - 10, 10); });

    cv::Mat scene, gray, edges;
    for (int i = 0; viewer.is_open(); ++i) {
        make_scene(scene, i);
        cv::cvtColor(scene, gray, cv::COLOR_BGR2GRAY);
        cv::GaussianBlur(gray, gray, {5, 5}, 1.2);
        cv::Canny(gray, edges, threshold, threshold * 2);
        viewer.show(edges);
        viewer.set_overlay_text("threshold", cv::format("Canny threshold %d   (+ / -)", threshold));
        viewer.wait_key(20);
    }
    std::printf("edges thread finished\n");
}

void blob_tracking() {
    fv::Viewer viewer("blobs", {800, 600});
    viewer.show_fps();
    viewer.on_mouse([](const fv::MouseEvent& e) {
        if (e.action == fv::MouseAction::PRESS)
            std::printf("blobs window: %s click at (%.0f, %.0f)\n",
                        e.button == Qt::LeftButton ? "left" : "right", e.world_pos.x, e.world_pos.y);
        std::fflush(stdout);
    });

    bool paused = false;
    int i = 0;
    cv::Mat scene, hsv, mask;
    while (viewer.is_open()) {
        if (!paused) {
            make_scene(scene, i++);
            cv::cvtColor(scene, hsv, cv::COLOR_BGR2HSV);
            cv::inRange(hsv, cv::Scalar(0, 90, 80), cv::Scalar(180, 255, 255), mask);
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            for (const auto& c : contours)
                if (cv::contourArea(c) > 500) cv::rectangle(scene, cv::boundingRect(c), {0, 0, 255}, 2);
            viewer.show(scene);
        }
        viewer.set_overlay_text("state", paused ? "PAUSED (Space)" : "running (Space to pause)");

        if (viewer.wait_key(30) == Qt::Key_Space) paused = !paused;
    }
    std::printf("blobs thread finished\n");
}

} // namespace

int main() {
    std::thread edges(edge_detection);
    std::thread blobs(blob_tracking);
    edges.join();
    blobs.join();
}
