// demo_keys — key bindings, wait_key() return values, mouse clicks and viewport reporting.
//
// Try:
//   Left click          drop a marker at that world position
//   Right click         remove the nearest marker
//   Double click        print the position to the terminal
//   1 / 2 / 3           marker colour          G   toggle grid       C   clear markers
//   Ctrl+S              save the image to demo_keys.png
//   Space               not bound: returned by wait_key() and handled in the loop
//   Ctrl+H              help (lists the bindings above)
//   Zoom and pan        watch the viewport readout in the bottom-left corner
#include <fv/viewer.h>
#include <QKeySequence>
#include <cmath>
#include <cstdio>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace {

struct Marker {
    cv::Point2d pos;
    cv::Scalar colour;
};

}  // namespace

// Forward declarations of helper functions
static void render(cv::Mat& out, const std::vector<Marker>& markers, bool grid, bool inverted);

int main() {
    fv::Viewer viewer("demo_keys", {1280, 800});

    std::vector<Marker> markers;
    cv::Scalar colour(0, 200, 255);
    bool grid = true;
    bool inverted = false;
    bool dirty = true;  // plain bools are fine: callbacks run on this thread, inside wait_key()
    cv::Mat image;

    viewer.bind(Qt::Key_1, "Marker colour: orange", [&] { colour = {0, 200, 255}; });
    viewer.bind(Qt::Key_2, "Marker colour: green", [&] { colour = {80, 220, 80}; });
    viewer.bind(Qt::Key_3, "Marker colour: magenta", [&] { colour = {220, 60, 220}; });
    viewer.bind(Qt::Key_G, "Toggle grid", [&] {
        grid = !grid;
        dirty = true;
    });
    viewer.bind(Qt::Key_C, "Clear markers", [&] {
        markers.clear();
        dirty = true;
    });
    viewer.bind("Ctrl+S", "Save image to demo_keys.png", [&] {
        cv::imwrite("demo_keys.png", image);
        viewer.set_overlay_text("status", "saved demo_keys.png", Qt::TopRightCorner);
    });

    viewer.on_mouse([&](const fv::MouseEvent& e) {
        const bool inside = e.world_pos.x >= 0 && e.world_pos.y >= 0 && e.world_pos.x < 1600 &&
                            e.world_pos.y < 1000;
        if (e.action == fv::MouseAction::PRESS && e.button == Qt::LeftButton && inside) {
            markers.push_back({e.world_pos, colour});
            dirty = true;
        } else if (e.action == fv::MouseAction::PRESS && e.button == Qt::RightButton &&
                   !markers.empty()) {
            // Nearest marker, measured in screen pixels so it feels the same at any zoom.
            size_t best = 0;
            double best_d = 1e300;
            for (size_t i = 0; i < markers.size(); ++i) {
                const cv::Point2d s = viewer.world_to_screen(markers[i].pos);
                const double d = std::hypot(s.x - e.screen_pos.x, s.y - e.screen_pos.y);
                if (d < best_d) {
                    best_d = d;
                    best = i;
                }
            }
            if (best_d < 30) {
                markers.erase(markers.begin() + static_cast<long>(best));
                dirty = true;
            }
        } else if (e.action == fv::MouseAction::DOUBLE_CLICK) {
            std::printf("double click: screen (%.0f, %.0f)  world (%.1f, %.1f)\n", e.screen_pos.x,
                        e.screen_pos.y, e.world_pos.x, e.world_pos.y);
        }
    });

    viewer.on_viewport_changed([&](const fv::Viewport& v) {
        viewer.set_overlay_text(
            "viewport",
            cv::format("zoom %.2fx   view x %.0f..%.0f  y %.0f..%.0f", 1.0 / v.world_per_screen_px,
                       v.world_rect.x, v.world_rect.x + v.world_rect.width, v.world_rect.y,
                       v.world_rect.y + v.world_rect.height),
            Qt::BottomLeftCorner);
    });

    viewer.set_overlay_text("hint", "Click to add markers.  Ctrl+H: help", Qt::TopLeftCorner);

    while (viewer.is_open()) {
        if (dirty) {
            render(image, markers, grid, inverted);
            viewer.show(image);
            viewer.set_overlay_text("count", cv::format("%zu markers", markers.size()),
                                    Qt::BottomRightCorner);
            dirty = false;
        }

        const fv::Key k = viewer.wait_key(50);
        if (k == Qt::Key_Space) {
            inverted = !inverted;  // handled in the loop rather than via bind()
            dirty = true;
        } else if (k) {
            const std::string name = QKeySequence(k.combination()).toString().toStdString();
            viewer.set_overlay_text("status", "unbound key: " + name, Qt::TopRightCorner);
        }
    }
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
static void render(cv::Mat& out, const std::vector<Marker>& markers, bool grid, bool inverted) {
    out.create(1000, 1600, CV_8UC3);
    out.setTo(inverted ? cv::Scalar(220, 220, 220) : cv::Scalar(40, 35, 30));
    const cv::Scalar line_colour = inverted ? cv::Scalar(150, 150, 150) : cv::Scalar(80, 75, 70);
    if (grid) {
        for (int x = 0; x < out.cols; x += 100)
            cv::line(out, {x, 0}, {x, out.rows - 1}, line_colour, 1);
        for (int y = 0; y < out.rows; y += 100)
            cv::line(out, {0, y}, {out.cols - 1, y}, line_colour, 1);
        for (int y = 0; y < out.rows; y += 100)
            for (int x = 0; x < out.cols; x += 100)
                cv::putText(out, cv::format("%d,%d", x, y), {x + 3, y + 12}, cv::FONT_HERSHEY_PLAIN,
                            0.8, line_colour, 1, cv::LINE_AA);
    }
    for (const Marker& m : markers) {
        const cv::Point p(cvRound(m.pos.x), cvRound(m.pos.y));
        cv::circle(out, p, 10, m.colour, 2, cv::LINE_AA);
        cv::drawMarker(out, p, m.colour, cv::MARKER_CROSS, 8, 1, cv::LINE_AA);
    }
}
