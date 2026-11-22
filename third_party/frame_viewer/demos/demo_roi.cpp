// demo_roi — drawing, moving and resizing ROI boxes with mouse drags.
//
// The boxes are set_overlay_box() overlays: they follow zoom and pan, but their lines stay the
// same width on screen at any zoom. Box edges snap to image pixel boundaries.
//
// Try:
//   Left drag on empty space     draw a new box
//   Left drag inside a box       move it
//   Left drag near a corner      resize it
//   Right click on a box         delete it
//   Del                          delete the selected box      C   clear all boxes
//   Wheel / Ctrl + left drag     zoom and pan: the box lines keep their width
#include <fv/viewer.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

static const cv::Size IMAGE_SIZE(4000, 3000);
// how close a press must be to a corner, in screen pixels
static constexpr double CORNER_GRAB_PX = 12;

struct Roi {
    std::string id;   // overlay id
    cv::Rect2d rect;  // world = image pixels
};

enum class DragMode : std::uint8_t { NONE, CREATE, MOVE, RESIZE };

// Forward declarations of helper functions
static cv::Mat make_scene();
static cv::Point2d snap_to_image(cv::Point2d world_pos);
static std::string describe(const Roi& roi);

int main() {
    fv::Viewer viewer("demo_roi", {1280, 800});
    viewer.show(make_scene());

    // Plain variables are fine: callbacks run on this thread, inside wait_until_closed()
    std::vector<Roi> rois;
    int selected = -1;  // index into rois, -1 = none
    int next_id = 1;
    DragMode mode = DragMode::NONE;
    // CREATE / RESIZE: the fixed corner. MOVE: cursor offset from the box's top-left
    cv::Point2d anchor;

    // Pushes every box to the viewer, highlighting the selected one
    auto sync_overlays = [&] {
        for (int i = 0; i < static_cast<int>(rois.size()); ++i) {
            const bool is_selected = i == selected;
            fv::Box box;
            box.rect = rois[i].rect;
            box.colour = is_selected ? cv::Scalar(0, 220, 255) : cv::Scalar(80, 220, 80);
            box.fill_opacity = is_selected ? 0.25 : 0.1;
            viewer.set_overlay_box(rois[i].id, box);
        }
        viewer.set_overlay_text("info", selected < 0 ? "no box selected" : describe(rois[selected]),
                                Qt::BottomLeftCorner);
    };

    auto remove_roi = [&](int index) {
        viewer.remove_overlay(rois[index].id);
        rois.erase(rois.begin() + index);
        selected = -1;
        mode = DragMode::NONE;
    };

    // Topmost (last added) box containing the point, or -1
    auto find_box = [&](cv::Point2d world_pos) {
        for (int i = static_cast<int>(rois.size()) - 1; i >= 0; --i)
            if (rois[i].rect.contains(world_pos)) return i;
        return -1;
    };

    // Starts a drag: resize if a corner was grabbed, else move if inside a box, else draw a new box
    auto start_drag = [&](const fv::MouseEvent& e) {
        // Corners are hit-tested on screen, so they're as easy to grab at any zoom
        for (int i = static_cast<int>(rois.size()) - 1; i >= 0; --i) {
            const cv::Rect2d& r = rois[i].rect;
            const cv::Point2d corners[4] = {
                r.tl(), {r.x + r.width, r.y}, r.br(), {r.x, r.y + r.height}};
            for (int c = 0; c < 4; ++c) {
                const cv::Point2d s = viewer.world_to_screen(corners[c]);
                if (std::hypot(s.x - e.screen_pos.x, s.y - e.screen_pos.y) > CORNER_GRAB_PX)
                    continue;
                mode = DragMode::RESIZE;
                selected = i;
                anchor = corners[(c + 2) % 4];
                return;
            }
        }

        if (const int i = find_box(e.world_pos); i >= 0) {
            mode = DragMode::MOVE;
            selected = i;
            anchor = e.world_pos - rois[i].rect.tl();
            return;
        }

        mode = DragMode::CREATE;
        anchor = snap_to_image(e.world_pos);
        rois.push_back(
            {.id = "roi" + std::to_string(next_id++), .rect = cv::Rect2d(anchor, anchor)});
        selected = static_cast<int>(rois.size()) - 1;
    };

    auto continue_drag = [&](cv::Point2d world_pos) {
        cv::Rect2d& r = rois[selected].rect;
        if (mode != DragMode::MOVE) {
            r = cv::Rect2d(
                anchor, snap_to_image(world_pos));  // normalises, so dragging past the anchor works
            return;
        }
        // Keep the whole box inside the image
        r.x = std::clamp(std::round(world_pos.x - anchor.x), 0.0, IMAGE_SIZE.width - r.width);
        r.y = std::clamp(std::round(world_pos.y - anchor.y), 0.0, IMAGE_SIZE.height - r.height);
    };

    auto finish_drag = [&] {
        const DragMode finished = mode;
        mode = DragMode::NONE;
        if (finished == DragMode::CREATE && rois[selected].rect.area() <= 0) {
            remove_roi(selected);  // a click without a drag doesn't make a box
            return;
        }
        std::printf("%s\n", describe(rois[selected]).c_str());
    };

    viewer.on_mouse([&](const fv::MouseEvent& e) {
        if (e.action == fv::MouseAction::PRESS && e.button == Qt::RightButton) {
            if (const int i = find_box(e.world_pos); i >= 0) remove_roi(i);
        } else if (e.action == fv::MouseAction::PRESS && e.button == Qt::LeftButton) {
            start_drag(e);
        } else if (e.action == fv::MouseAction::MOVE && mode != DragMode::NONE) {
            continue_drag(e.world_pos);
        } else if (e.action == fv::MouseAction::RELEASE && e.button == Qt::LeftButton &&
                   mode != DragMode::NONE) {
            finish_drag();
        } else {
            return;
        }
        sync_overlays();
    });

    viewer.bind(Qt::Key_Delete, "Delete selected box", [&] {
        if (selected >= 0) remove_roi(selected);
        sync_overlays();
    });
    viewer.bind(Qt::Key_C, "Clear all boxes", [&] {
        while (!rois.empty())
            remove_roi(static_cast<int>(rois.size()) - 1);
        sync_overlays();
    });

    viewer.on_viewport_changed([&](const fv::Viewport& v) {
        viewer.set_overlay_text("zoom", cv::format("zoom %.2fx", 1.0 / v.world_per_screen_px),
                                Qt::BottomRightCorner);
    });

    viewer.set_overlay_text("hint",
                            "Drag to draw a box, drag a box to move it, drag a corner to resize.\n"
                            "Right click deletes.  Ctrl+H: help",
                            Qt::TopLeftCorner);
    sync_overlays();
    viewer.wait_until_closed();
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
/**
 * Draw random coloured discs on a labelled 250 px grid, so there's something to box at every
 * zoom.
 */
static cv::Mat make_scene() {
    cv::Mat scene(IMAGE_SIZE, CV_8UC3, cv::Scalar(45, 40, 35));
    cv::RNG rng(7);
    for (int i = 0; i < 400; ++i) {
        const cv::Point centre(rng.uniform(0, IMAGE_SIZE.width), rng.uniform(0, IMAGE_SIZE.height));
        const cv::Scalar colour(rng.uniform(60, 230), rng.uniform(60, 230), rng.uniform(60, 230));
        cv::circle(scene, centre, rng.uniform(5, 90), colour, cv::FILLED, cv::LINE_AA);
    }

    const cv::Scalar line_colour(110, 105, 100);
    for (int x = 0; x < IMAGE_SIZE.width; x += 250)
        cv::line(scene, {x, 0}, {x, IMAGE_SIZE.height - 1}, line_colour, 1);
    for (int y = 0; y < IMAGE_SIZE.height; y += 250)
        cv::line(scene, {0, y}, {IMAGE_SIZE.width - 1, y}, line_colour, 1);
    for (int y = 0; y < IMAGE_SIZE.height; y += 250)
        for (int x = 0; x < IMAGE_SIZE.width; x += 250)
            cv::putText(scene, cv::format("%d,%d", x, y), {x + 4, y + 16}, cv::FONT_HERSHEY_PLAIN,
                        1.0, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
    return scene;
}

/** Round to the nearest pixel boundary inside the image, so box edges line up with pixels. */
static cv::Point2d snap_to_image(cv::Point2d world_pos) {
    return {std::clamp(std::round(world_pos.x), 0.0, static_cast<double>(IMAGE_SIZE.width)),
            std::clamp(std::round(world_pos.y), 0.0, static_cast<double>(IMAGE_SIZE.height))};
}

static std::string describe(const Roi& roi) {
    const cv::Rect2d& r = roi.rect;
    return cv::format("%s: x %.0f  y %.0f  w %.0f  h %.0f", roi.id.c_str(), r.x, r.y, r.width,
                      r.height);
}
