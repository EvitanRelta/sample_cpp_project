// demo_canvas — dragging images and shapes around a canvas.
//
// Every item is its own frame. Dragging calls move_frame(), which only changes where the frame
// sits (and its layer), so the image isn't resubmitted and stays full quality while it moves.
// Shapes are drawn into small opaque tiles, so they move as cards. Items can be rotated: the viewer
// rotates the frame itself, so items below stay visible around its corners.
//
// Usage: demo_canvas [image files...]   (files are added as extra items)
//
// Try:
//   Left drag on an item         move it, even off the canvas (it comes to the front and gets a
//                                selection outline)
//   Q / E                        rotate the selected item anticlockwise / clockwise
//   Ctrl+0                       refit the view to all items
//   Left click on the canvas     deselect
//   R                            scatter the items, with random rotations
//   Wheel / Ctrl + left drag     zoom and pan
#include <fv/viewer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

static const cv::Size CANVAS_SIZE(3000, 2000);
static constexpr fv::FrameId CANVAS_ID = 0;
static constexpr int TILE_PX = 300;
// longest side of an item loaded from a file, in world units
static constexpr double MAX_FILE_ITEM_SIZE = 600;

struct Item {
    fv::FrameId id;
    cv::Rect2d rect;  // placement before rotation
    int layer;
    double angle_deg = 0.0;  // clockwise about the rect centre
};

// Forward declarations of helper functions
static cv::Mat make_canvas();
static std::vector<cv::Mat> make_tiles();
static cv::Point2d clamp_to_canvas(cv::Point2d top_left, cv::Size2d size);
static cv::Point2d rotate_about(cv::Point2d p, cv::Point2d centre, double angle_deg);

int main(int argc, char** argv) {
    fv::Viewer viewer("demo_canvas", {1280, 800});
    // The background covers the whole canvas, so the initial fitted view is the whole canvas
    viewer.set_frame(CANVAS_ID, make_canvas(), cv::Rect2d({0, 0}, CANVAS_SIZE), 0);

    std::vector<cv::Mat> images = make_tiles();
    std::vector<cv::Size2d> sizes(images.size(), cv::Size2d(TILE_PX, TILE_PX));
    for (int i = 1; i < argc; ++i) {
        cv::Mat image = cv::imread(argv[i], cv::IMREAD_COLOR);
        if (image.empty()) {
            std::fprintf(stderr, "can't read %s\n", argv[i]);
            continue;
        }
        const double scale = std::min(1.0, MAX_FILE_ITEM_SIZE / std::max(image.cols, image.rows));
        images.push_back(image);
        sizes.emplace_back(image.cols * scale, image.rows * scale);
    }

    // Items start in rows; layers 1, 2, ... put them above the canvas in the order added
    std::vector<Item> items;
    int top_layer = 0;
    cv::Point2d next_pos(100, 100);
    for (size_t i = 0; i < images.size(); ++i) {
        if (next_pos.x + sizes[i].width > CANVAS_SIZE.width) next_pos = {100, next_pos.y + 650};
        const cv::Point2d tl = clamp_to_canvas(next_pos, sizes[i]);
        items.push_back({.id = i + 1, .rect = cv::Rect2d(tl, sizes[i]), .layer = ++top_layer});
        viewer.set_frame(items.back().id, images[i], items.back().rect, items.back().layer);
        next_pos.x += sizes[i].width + 80;
    }
    images.clear();  // set_frame copied them

    // Plain variables are fine: callbacks run on this thread, inside wait_until_closed()
    int selected = -1;  // index into items, -1 = none
    bool dragging = false;
    cv::Point2d grab_offset;  // cursor position relative to the dragged item's top-left

    auto show_selection = [&] {
        if (selected < 0) {
            viewer.remove_overlay("selection");
            return;
        }
        // Overlays are in world coordinates, so outline the item's rotated corners
        const Item& item = items[selected];
        const cv::Point2d centre = (item.rect.tl() + item.rect.br()) * 0.5;
        std::vector<cv::Point2d> corners;
        for (const cv::Point2d corner :
             {item.rect.tl(), cv::Point2d(item.rect.x + item.rect.width, item.rect.y),
              item.rect.br(), cv::Point2d(item.rect.x, item.rect.y + item.rect.height)})
            corners.push_back(rotate_about(corner, centre, item.angle_deg));
        viewer.set_overlay_polygon("selection", corners, cv::Scalar(255, 160, 0), 3, 0.15);
    };

    viewer.on_mouse([&](const fv::MouseEvent& e) {
        if (e.action == fv::MouseAction::PRESS && e.button == Qt::LeftButton) {
            // Topmost item under the cursor: undo each item's rotation, then test its rect
            selected = -1;
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const cv::Rect2d& r = items[i].rect;
                const cv::Point2d local =
                    rotate_about(e.world_pos, (r.tl() + r.br()) * 0.5, -items[i].angle_deg);
                if (r.contains(local) && (selected < 0 || items[i].layer > items[selected].layer))
                    selected = i;
            }
            dragging = selected >= 0;
            if (dragging) {
                Item& item = items[selected];
                grab_offset = e.world_pos - item.rect.tl();
                item.layer = ++top_layer;  // bring to the front
                viewer.move_frame(item.id, item.rect, item.layer);
                // Pin the camera where it is, which turns off auto-fit. Otherwise dragging an item
                // off the canvas grows the content bounds and the view zooms out under the cursor.
                viewer.show_world_rect(viewer.current_viewport().world_rect);
            }
            show_selection();
        } else if (e.action == fv::MouseAction::MOVE && dragging) {
            Item& item = items[selected];
            // No clamp: items can be dragged off the canvas. Ctrl+0 refits to include them.
            const cv::Point2d tl = e.world_pos - grab_offset;
            item.rect.x = tl.x;
            item.rect.y = tl.y;
            viewer.move_frame(item.id, item.rect);
            show_selection();
        } else if (e.action == fv::MouseAction::RELEASE && e.button == Qt::LeftButton) {
            dragging = false;
        }
    });

    viewer.bind(Qt::Key_R, "Scatter the items", [&] {
        cv::RNG rng(cv::getTickCount());
        for (Item& item : items) {
            const cv::Point2d tl =
                clamp_to_canvas({rng.uniform(0.0, CANVAS_SIZE.width - item.rect.width),
                                 rng.uniform(0.0, CANVAS_SIZE.height - item.rect.height)},
                                item.rect.size());
            item.rect.x = tl.x;
            item.rect.y = tl.y;
            item.angle_deg = rng.uniform(-30.0, 30.0);
            viewer.move_frame(item.id, item.rect, {}, item.angle_deg);
        }
        show_selection();
    });

    auto rotate_selected = [&](double step_deg) {
        if (selected < 0) return;
        Item& item = items[selected];
        item.angle_deg = std::fmod(item.angle_deg + step_deg, 360.0);
        viewer.move_frame(item.id, item.rect, {}, item.angle_deg);
        show_selection();
    };
    viewer.bind(Qt::Key_Q, "Rotate the selected item anticlockwise", [&] { rotate_selected(-15); });
    viewer.bind(Qt::Key_E, "Rotate the selected item clockwise", [&] { rotate_selected(15); });

    viewer.set_overlay_text(
        "hint", "Drag items around the canvas.  Q/E: rotate   R: scatter   Ctrl+H: help",
        Qt::TopLeftCorner);
    viewer.wait_until_closed();
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
/** Draw a light background with a dot grid every 100 world units. */
static cv::Mat make_canvas() {
    cv::Mat canvas(CANVAS_SIZE, CV_8UC3, cv::Scalar(235, 232, 228));
    for (int y = 50; y < CANVAS_SIZE.height; y += 100)
        for (int x = 50; x < CANVAS_SIZE.width; x += 100)
            cv::circle(canvas, {x, y}, 3, cv::Scalar(190, 185, 180), cv::FILLED, cv::LINE_AA);
    cv::rectangle(canvas, cv::Rect({0, 0}, CANVAS_SIZE), cv::Scalar(150, 145, 140), 4);
    return canvas;
}

/** Generate "images" and shapes, each on a TILE_PX square card with its name. */
static std::vector<cv::Mat> make_tiles() {
    const cv::Point centre(TILE_PX / 2, TILE_PX / 2);
    auto new_tile = [](cv::Scalar background) {
        return cv::Mat(TILE_PX, TILE_PX, CV_8UC3, background);
    };
    auto label = [](cv::Mat& tile, const std::string& text) {
        cv::putText(tile, text, {12, TILE_PX - 14}, cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(40, 40, 40), 2, cv::LINE_AA);
        cv::rectangle(tile, cv::Rect(0, 0, TILE_PX, TILE_PX), cv::Scalar(90, 90, 90), 2);
    };
    std::vector<cv::Mat> tiles;

    // Hue gradient
    cv::Mat hsv(TILE_PX, TILE_PX, CV_8UC3);
    for (int y = 0; y < TILE_PX; ++y)
        for (int x = 0; x < TILE_PX; ++x)
            hsv.at<cv::Vec3b>(y, x) = cv::Vec3b(x * 180 / TILE_PX, 255 - y * 155 / TILE_PX, 255);
    cv::Mat gradient;
    cv::cvtColor(hsv, gradient, cv::COLOR_HSV2BGR);
    label(gradient, "gradient");
    tiles.push_back(gradient);

    // Checkerboard
    cv::Mat checker = new_tile(cv::Scalar(250, 250, 250));
    for (int y = 0; y < TILE_PX; y += 25)
        for (int x = (y / 25) % 2 * 25; x < TILE_PX; x += 50)
            cv::rectangle(checker, cv::Rect(x, y, 25, 25), cv::Scalar(60, 60, 60), cv::FILLED);
    cv::rectangle(checker, cv::Rect(0, TILE_PX - 44, TILE_PX, 44), cv::Scalar(250, 250, 250),
                  cv::FILLED);
    label(checker, "checker");
    tiles.push_back(checker);

    // Concentric rings, which show the full-quality downscale when zoomed out
    cv::Mat rings = new_tile(cv::Scalar(255, 255, 255));
    for (int r = 4; r < TILE_PX / 2; r += 6)
        cv::circle(rings, centre, r, cv::Scalar(120, 40, 20), 2, cv::LINE_AA);
    label(rings, "rings");
    tiles.push_back(rings);

    cv::Mat circle = new_tile(cv::Scalar(225, 245, 255));
    cv::circle(circle, centre, 100, cv::Scalar(40, 80, 230), cv::FILLED, cv::LINE_AA);
    label(circle, "circle");
    tiles.push_back(circle);

    cv::Mat square = new_tile(cv::Scalar(230, 255, 230));
    cv::rectangle(square, cv::Rect(60, 50, 180, 180), cv::Scalar(60, 170, 40), cv::FILLED,
                  cv::LINE_AA);
    label(square, "square");
    tiles.push_back(square);

    cv::Mat triangle = new_tile(cv::Scalar(255, 235, 225));
    const std::vector<cv::Point> corners = {{150, 30}, {270, 240}, {30, 240}};
    cv::fillConvexPoly(triangle, corners, cv::Scalar(200, 120, 30), cv::LINE_AA);
    label(triangle, "triangle");
    tiles.push_back(triangle);

    cv::Mat star = new_tile(cv::Scalar(220, 250, 250));
    std::vector<cv::Point> points;
    for (int i = 0; i < 10; ++i) {
        const double angle = CV_PI / 5 * i - CV_PI / 2;
        const double radius = i % 2 == 0 ? 115 : 48;
        points.emplace_back(cvRound(centre.x + radius * std::cos(angle)),
                            cvRound(centre.y - 10 + radius * std::sin(angle)));
    }
    cv::fillPoly(star, std::vector<std::vector<cv::Point>>{points}, cv::Scalar(30, 190, 230),
                 cv::LINE_AA);
    label(star, "star");
    tiles.push_back(star);

    return tiles;
}

/**
 * Return the top-left position nearest `top_left` that keeps an item of `size` fully on the
 * canvas.
 */
static cv::Point2d clamp_to_canvas(cv::Point2d top_left, cv::Size2d size) {
    return {std::clamp(top_left.x, 0.0, std::max(0.0, CANVAS_SIZE.width - size.width)),
            std::clamp(top_left.y, 0.0, std::max(0.0, CANVAS_SIZE.height - size.height))};
}

/** Rotate `p` about `centre`, clockwise on screen like fv::Viewer::set_frame()'s angle_deg. */
static cv::Point2d rotate_about(cv::Point2d p, cv::Point2d centre, double angle_deg) {
    const double rad = angle_deg * CV_PI / 180.0;
    const cv::Point2d d = p - centre;
    return centre + cv::Point2d(d.x * std::cos(rad) - d.y * std::sin(rad),
                                d.x * std::sin(rad) + d.y * std::cos(rad));
}
