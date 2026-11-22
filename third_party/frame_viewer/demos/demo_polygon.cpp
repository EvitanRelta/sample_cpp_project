// demo_polygon — drawing polygons with set_overlay_polygon() and saving them as a bitmask.
//
// The polygons are overlays: they follow zoom and pan, but their lines stay the same width on
// screen at any zoom. The mask is plain OpenCV (cv::fillPoly), made from the vertex list.
//
// Try:
//   Left click                   place a vertex (hold and drag to adjust it before releasing)
//   Space                        close the polygon (joins the last vertex to the first)
//   Ctrl+S                       save the closed polygons as polygon_mask.png (255 = inside)
//   Backspace                    undo the last vertex, or the last polygon      C   clear all
//   Wheel / Ctrl + left drag     zoom and pan: the lines keep their width
#include <fv/viewer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

static const cv::Size IMAGE_SIZE(3000, 2000);
static const char* MASK_PATH = "polygon_mask.png";

// Forward declarations of helper functions
static cv::Mat make_scene();
static cv::Point2d clamp_to_image(cv::Point2d world_pos);
static cv::Mat make_mask(const std::vector<std::vector<cv::Point2d>>& polygons);

int main() {
    fv::Viewer viewer("demo_polygon", {1280, 800});
    viewer.show(make_scene());

    // Plain variables are fine: callbacks run on this thread, inside wait_until_closed()
    std::vector<std::vector<cv::Point2d>> polygons;  // closed; overlay ids "poly0", "poly1", ...
    std::vector<cv::Point2d> current;                // open, being placed; overlay id "current"
    bool dragging_vertex = false;

    // Pushes every polygon to the viewer, plus a status line
    auto sync_overlays = [&] {
        for (int i = 0; i < static_cast<int>(polygons.size()); ++i)
            viewer.set_overlay_polygon("poly" + std::to_string(i), polygons[i],
                                       cv::Scalar(80, 220, 80), 2, 0.25);
        viewer.set_overlay_polygon("current", current, cv::Scalar(0, 220, 255), 2, 0.0, false);
        viewer.set_overlay_text(
            "info",
            cv::format("%zu polygon(s), %zu vertex(es) placed", polygons.size(), current.size()),
            Qt::BottomLeftCorner);
    };

    viewer.on_mouse([&](const fv::MouseEvent& e) {
        if (e.action == fv::MouseAction::PRESS && e.button == Qt::LeftButton) {
            current.push_back(clamp_to_image(e.world_pos));
            dragging_vertex = true;
        } else if (e.action == fv::MouseAction::MOVE && dragging_vertex) {
            current.back() = clamp_to_image(e.world_pos);
        } else if (e.action == fv::MouseAction::RELEASE && e.button == Qt::LeftButton) {
            dragging_vertex = false;
            return;
        } else {
            return;
        }
        sync_overlays();
    });

    viewer.bind(Qt::Key_Space, "Close the polygon", [&] {
        if (current.size() < 3) {
            std::printf("a polygon needs at least 3 vertices\n");
            return;
        }
        polygons.push_back(std::move(current));
        current.clear();
        dragging_vertex = false;
        sync_overlays();
    });
    viewer.bind(Qt::Key_Backspace, "Undo the last vertex or polygon", [&] {
        if (!current.empty()) {
            current.pop_back();
            dragging_vertex = false;
        } else if (!polygons.empty()) {
            polygons.pop_back();
            viewer.remove_overlay("poly" + std::to_string(polygons.size()));
        }
        sync_overlays();
    });
    viewer.bind(Qt::Key_C, "Clear all polygons", [&] {
        for (int i = 0; i < static_cast<int>(polygons.size()); ++i)
            viewer.remove_overlay("poly" + std::to_string(i));
        polygons.clear();
        current.clear();
        dragging_vertex = false;
        sync_overlays();
    });
    viewer.bind("Ctrl+S", "Save the bitmask", [&] {
        if (polygons.empty()) {
            std::printf("nothing to save: close a polygon with Space first\n");
            return;
        }
        const cv::Mat mask = make_mask(polygons);
        if (!cv::imwrite(MASK_PATH, mask)) {
            std::printf("failed to write %s\n", MASK_PATH);
            return;
        }
        std::printf("saved %s (%d x %d, %d pixels inside)\n", MASK_PATH, mask.cols, mask.rows,
                    cv::countNonZero(mask));
    });

    viewer.set_overlay_text("hint",
                            "Click to place vertices, Space closes the polygon, Ctrl+S saves the "
                            "mask.\nBackspace undoes, C clears.  Ctrl+H: help",
                            Qt::TopLeftCorner);
    sync_overlays();
    viewer.wait_until_closed();
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
/** Draw random coloured discs on a grey background, so there's something to outline. */
static cv::Mat make_scene() {
    cv::Mat scene(IMAGE_SIZE, CV_8UC3, cv::Scalar(45, 40, 35));
    cv::RNG rng(11);
    for (int i = 0; i < 150; ++i) {
        const cv::Point centre(rng.uniform(0, IMAGE_SIZE.width), rng.uniform(0, IMAGE_SIZE.height));
        const cv::Scalar colour(rng.uniform(60, 230), rng.uniform(60, 230), rng.uniform(60, 230));
        cv::circle(scene, centre, rng.uniform(10, 150), colour, cv::FILLED, cv::LINE_AA);
    }
    return scene;
}

/** Keep a vertex on the image (world = image pixels for show()). */
static cv::Point2d clamp_to_image(cv::Point2d world_pos) {
    return {std::clamp(world_pos.x, 0.0, static_cast<double>(IMAGE_SIZE.width)),
            std::clamp(world_pos.y, 0.0, static_cast<double>(IMAGE_SIZE.height))};
}

/** Fill the polygons into an 8-bit image-sized mask: 255 inside, 0 outside. */
static cv::Mat make_mask(const std::vector<std::vector<cv::Point2d>>& polygons) {
    // fillPoly takes fixed-point vertices with `shift` fractional bits, so sub-pixel clicks
    // count. Pixel (x, y) covers world [x, x + 1), so its centre, where fillPoly samples, is
    // x + 0.5.
    const int shift = 8;
    cv::Mat mask = cv::Mat::zeros(IMAGE_SIZE, CV_8UC1);
    for (const auto& polygon : polygons) {
        std::vector<cv::Point> contour;
        for (const cv::Point2d& p : polygon)
            contour.emplace_back(static_cast<int>(std::lround((p.x - 0.5) * (1 << shift))),
                                 static_cast<int>(std::lround((p.y - 0.5) * (1 << shift))));
        // One call per polygon, so overlapping polygons add up instead of cancelling out
        cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{contour}, cv::Scalar(255),
                     cv::LINE_8, shift);
    }
    return mask;
}
