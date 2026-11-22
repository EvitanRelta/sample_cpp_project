// demo_boxes — frame-attached boxes and labels, submitted with each image, plus two world-space
// overlay boxes (set_overlay_box()) placed across the image edge: their outlines, fill and labels
// are clipped to the window, not the image.
// B toggles boxes; L toggles labels. Zoom and pan keep outlines and text the same screen size.
#include <fv/viewer.h>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <string>
#include <utility>
#include <vector>

int main() {
    const cv::Size frame_size(1920, 1080);
    // Different tag colours exercise text contrast: red, medium green, and blue (BGR).
    const cv::Scalar box_colours[] = {{0, 0, 255}, {0, 160, 0}, {255, 0, 0}};
    fv::Viewer viewer("demo_boxes", {1280, 720});
    fv::BufferPool pool(frame_size, CV_8UC3);
    bool show_boxes = true;
    bool show_labels = true;
    viewer.bind(Qt::Key_B, "Toggle boxes", [&] { show_boxes = !show_boxes; });
    viewer.bind(Qt::Key_L, "Toggle labels", [&] { show_labels = !show_labels; });
    viewer.set_overlay_text("hint", "B: boxes   L: labels   Wheel: zoom   Ctrl+drag: pan");

    // Leave a margin around the image so the overlay boxes' outer parts are visible.
    viewer.show_world_rect(cv::Rect2d(-240, -135, 2400, 1350));

    int frame_num = 0;
    while (viewer.is_open()) {
        cv::Mat image = pool.acquire();
        image.setTo(cv::Scalar::all(35));
        std::vector<fv::Box> boxes;
        for (int i = 0; i < 3; ++i) {
            const double phase = frame_num * 0.025 + i * 2.0;
            const cv::Point centre(960 + static_cast<int>(650 * std::sin(phase)),
                                   540 + static_cast<int>(350 * std::cos(phase * 0.7)));
            cv::circle(image, centre, 65, cv::Scalar::all(160 + i * 30), cv::FILLED, cv::LINE_AA);
            if (show_boxes)
                boxes.push_back({cv::Rect2d(centre.x - 75, centre.y - 75, 150, 150), box_colours[i],
                                 2, show_labels ? "ball " + std::to_string(i + 1) : ""});
        }
        viewer.show_shared(image, std::move(boxes));

        // Overlay boxes in world coordinates, across the right and bottom-left image edges.
        if (show_boxes) {
            viewer.set_overlay_box("edge", {cv::Rect2d(1860, 60, 300, 260), cv::Scalar(0, 220, 255),
                                            2, show_labels ? "overlay across the edge" : ""});
            viewer.set_overlay_box("filled",
                                   {cv::Rect2d(-160, 780, 520, 380), cv::Scalar(255, 0, 255), 2,
                                    show_labels ? "filled overlay" : "", 0.3});
        } else {
            viewer.remove_overlay("edge");
            viewer.remove_overlay("filled");
        }
        ++frame_num;
        viewer.wait_key(16);
    }
}
