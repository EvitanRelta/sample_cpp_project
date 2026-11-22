// Run directly, with no display needed: QT_QPA_PLATFORM=offscreen ./build/bin/test_frame_boxes
#include <fv/viewer.h>
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QWidget>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>
#include "ui_thread.h"

// Forward declarations of helper functions
static QImage capture();
static int count_colour(const QImage& image, const QRect& area, const QColor& colour);
static int count_reddish(const QImage& image, const QRect& area);
static void check(bool condition, const char* message);

static int failures = 0;

int main() {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_SCALE_FACTOR", "1");
    fv::Viewer viewer("test_frame_boxes", {320, 240});
    cv::Mat image(240, 320, CV_8UC3, cv::Scalar::all(30));
    const QColor red(255, 0, 0);
    const QColor green(0, 255, 0);
    const QColor blue(0, 0, 255);
    const fv::Box box{cv::Rect2d(60, 70, 120, 100), cv::Scalar(0, 0, 255), 2, ""};
    std::vector<fv::Box> boxes{box};

    viewer.show_shared(image, boxes);
    QImage shown = capture();
    check(shown.size() == QSize(320, 240), "capture uses the requested window size");
    if (shown.size() != QSize(320, 240)) return 1;
    check(shown.pixelColor(100, 70) == red, "shared frame draws its box in BGR colour order");
    check(shown.pixelColor(100, 100) == QColor(30, 30, 30),
          "box does not fill or modify the image");
    check(image.at<cv::Vec3b>(70, 100) == cv::Vec3b(30, 30, 30), "source pixels remain unchanged");
    check(count_colour(shown, QRect(100, 65, 1, 11), red) == 2,
          "outline is two screen pixels wide");

    // Later shared submissions use a fresh buffer, like a BufferPool would, so debug builds don't
    // warn about resubmitting the buffer that's already shown.
    boxes[0].label = "box 1";
    viewer.show_shared(image.clone(), boxes);
    boxes.clear();
    shown = capture();
    const QRect label_area(60, 35, 120, 30);
    check(count_colour(shown, label_area, red) > 0, "label tag is drawn above the box");
    const int text_pixels = count_colour(shown, label_area, Qt::black);
    check(text_pixels > 0, "red label has black text, not just a coloured tag");
    check(shown.pixelColor(100, 70) == red, "caller can reuse the submitted box list");

    viewer.show_world_rect(cv::Rect2d(0, 0, 160, 120));
    shown = capture();
    check(shown.pixelColor(200, 140) == red, "box follows zoom");
    check(count_colour(shown, QRect(200, 135, 1, 11), red) == 2,
          "zoom preserves outline thickness");
    check(count_colour(shown, QRect(120, 105, 120, 30), Qt::black) == text_pixels,
          "zoom preserves text size");
    viewer.show_world_rect(cv::Rect2d(40, 30, 160, 120));
    shown = capture();
    check(shown.pixelColor(100, 80) == red, "box follows pan");
    viewer.show_world_rect(cv::Rect2d(0, 0, 320, 240));

    fv::Box labelled_box = box;
    labelled_box.label = "box 1";
    labelled_box.colour = cv::Scalar(0, 160, 0);
    viewer.show_shared(image.clone(), {labelled_box});
    check(count_colour(capture(), label_area, Qt::black) > 0,
          "medium-green label chooses higher-contrast black text");
    labelled_box.colour = cv::Scalar(255, 0, 0);
    viewer.show_shared(image.clone(), {labelled_box});
    check(count_colour(capture(), label_area, Qt::white) > 0,
          "blue label has white text on its dark tag");
    labelled_box.colour = cv::Scalar(0, 255, 0);
    viewer.show_shared(image.clone(), {labelled_box});
    check(count_colour(capture(), label_area, Qt::black) > 0,
          "bright-green label has black text on its light tag");

    // Check both sides of the black/white contrast crossover for neutral sRGB colours.
    labelled_box.colour = cv::Scalar::all(117);
    viewer.show_shared(image.clone(), {labelled_box});
    check(count_colour(capture(), label_area, Qt::white) > 0, "grey 117 label chooses white text");
    labelled_box.colour = cv::Scalar::all(118);
    viewer.show_shared(image.clone(), {labelled_box});
    check(count_colour(capture(), label_area, Qt::black) > 0, "grey 118 label chooses black text");

    // Leave a red label for the clearing check below.
    labelled_box.colour = box.colour;
    viewer.show_shared(image.clone(), {labelled_box});
    viewer.show_shared(image.clone());
    check(count_colour(capture(), QRect(0, 0, 320, 240), red) == 0,
          "plain shared submission clears old boxes and labels");
    viewer.show_shared(image.clone(), {box});
    viewer.show_shared(image.clone(), {});
    check(count_colour(capture(), QRect(0, 0, 320, 240), red) == 0,
          "empty box list removes old boxes");
    viewer.show(image, {box});
    image.setTo(cv::Scalar::all(90));  // show() owns a copy, so changing this buffer is safe.
    shown = capture();
    check(shown.pixelColor(100, 100) == QColor(30, 30, 30), "annotated show() copies the image");
    check(shown.pixelColor(100, 70) == red, "annotated show() draws its boxes");
    viewer.show(image);
    check(count_colour(capture(), QRect(0, 0, 320, 240), red) == 0,
          "plain copying submission clears old boxes");

    fv::Box green_box = box;
    green_box.colour = cv::Scalar(0, 255, 0);
    viewer.show_shared(image.clone(), {box, green_box});
    check(capture().pixelColor(100, 70) == green, "later boxes draw above earlier boxes");

    fv::Box clipped{cv::Rect2d(-10, 30, 80, 60), cv::Scalar(0, 0, 255), 2, "edge"};
    viewer.show_shared(image.clone(), {clipped});
    shown = capture();
    check(shown.pixelColor(0, 60) == QColor(90, 90, 90),
          "off-image box edge is clipped, not relocated");
    check(shown.pixelColor(10, 30) == red, "partially visible box still draws its visible edges");
    clipped.rect = cv::Rect2d(20, 0, 80, 60);
    viewer.show_shared(image.clone(), {clipped});
    check(count_colour(capture(), QRect(20, 1, 80, 25), Qt::black) > 0,
          "top-edge label text stays inside the image");

    cv::Mat small(120, 160, CV_8UC3, cv::Scalar::all(90));
    clipped.rect = cv::Rect2d(150, 80, 100, 100);
    viewer.show_shared(small, {clipped});
    shown = capture();
    check(count_colour(shown, QRect(160, 0, 160, 240), red) == 0,
          "annotations stay within the image horizontally");
    check(count_colour(shown, QRect(0, 120, 320, 120), red) == 0,
          "annotations stay within the image vertically");

    fv::Box huge{cv::Rect2d(-1e12, -1e12, 2e12, 2e12), cv::Scalar(0, 0, 255), 2, ""};
    fv::Box invalid = box;
    invalid.rect.x = std::numeric_limits<double>::infinity();
    viewer.show_shared(image.clone(), {huge, invalid});
    check(count_colour(capture(), QRect(0, 0, 320, 240), red) == 0,
          "distant and invalid edges do not create visible lines");

    viewer.set_overlay_box("persistent",
                           {cv::Rect2d(240, 30, 40, 40), cv::Scalar(255, 0, 0), 2, ""});
    viewer.show_shared(image.clone(), {box});
    viewer.clear_frames();
    shown = capture();
    check(count_colour(shown, QRect(0, 0, 320, 240), red) == 0,
          "clearing frames removes attached boxes");
    check(shown.pixelColor(260, 30) == blue,
          "clearing frames leaves persistent overlays unchanged");
    viewer.remove_overlay("persistent");

    // Box fill: inside the box only, blended by fill_opacity, and clipped to the image.
    fv::Box filled = box;
    filled.fill_opacity = 1.0;
    viewer.show_shared(image.clone(), {filled});  // grey 90 by now
    shown = capture();
    check(shown.pixelColor(100, 100) == red, "fill_opacity fills a frame-attached box");
    check(shown.pixelColor(200, 100) == QColor(90, 90, 90), "box fill stays inside the box");
    filled.fill_opacity = 0.5;
    viewer.show_shared(image.clone(), {filled});
    const QColor half = capture().pixelColor(100, 100);
    check(half.red() > 160 && half.red() < 185 && half.green() > 35 && half.green() < 55,
          "half fill_opacity blends with the image");
    filled.fill_opacity = 1.0;
    filled.rect = cv::Rect2d(100, 60, 100, 100);
    viewer.show_shared(small.clone(), {filled});
    check(count_colour(capture(), QRect(161, 0, 159, 240), red) == 0,
          "box fill is clipped to the image");

    // Overlay box labels are drawn like frame-attached ones, but clipped to the window instead.
    viewer.show_shared(image.clone());
    viewer.set_overlay_box("labelled", {box.rect, box.colour, 2, "overlay 1"});
    shown = capture();
    check(count_colour(shown, label_area, red) > 0, "overlay label tag is drawn above the box");
    check(count_colour(shown, label_area, Qt::black) > 0, "overlay label has contrasting text");
    viewer.show_shared(small.clone());
    viewer.set_overlay_box("labelled", {cv::Rect2d(140, 60, 100, 40), box.colour, 2, "overlay 1"});
    check(count_colour(capture(), QRect(165, 30, 150, 27), red) > 0,
          "overlay label extends past the image edge, within the window");
    viewer.set_overlay_box("labelled", {cv::Rect2d(200, -50, 80, 100), box.colour, 2, "top"});
    check(count_colour(capture(), QRect(205, 0, 70, 5), red) > 0,
          "top-edge overlay label stays inside the window");
    viewer.remove_overlay("labelled");
    check(count_colour(capture(), QRect(0, 0, 320, 240), red) == 0,
          "remove_overlay removes overlay boxes and their labels");

    // Polygon overlays: outline and fill in BGR colour order, open polylines unfilled, and
    // distant vertices don't produce stray lines.
    viewer.show_shared(image.clone());  // grey 90 by now
    viewer.show_world_rect(cv::Rect2d(0, 0, 320, 240));
    viewer.set_overlay_polygon("poly", {{40, 40}, {140, 40}, {140, 140}, {40, 140}},
                               cv::Scalar(255, 0, 0), 2, 1.0);
    shown = capture();
    check(shown.pixelColor(90, 90) == blue, "closed polygon is filled");
    check(shown.pixelColor(200, 90) == QColor(90, 90, 90), "polygon fill stays inside");
    viewer.set_overlay_polygon("poly", {{40, 40}, {140, 40}, {140, 140}}, cv::Scalar(0, 0, 255), 2,
                               1.0, false);
    shown = capture();
    check(shown.pixelColor(90, 40) == red, "open polyline draws its edges");
    check(shown.pixelColor(110, 70) == QColor(90, 90, 90), "open polyline is not filled");
    check(shown.pixelColor(90, 90) == QColor(90, 90, 90), "open polyline has no closing edge");
    viewer.set_overlay_polygon("poly", {{-1e12, 100}, {1e12, 100}, {1e12, 1e12}},
                               cv::Scalar(0, 0, 255), 2, 0.0, false);
    shown = capture();
    check(shown.pixelColor(160, 100) == red, "distant vertices still draw the visible edge");
    check(count_colour(shown, QRect(0, 0, 320, 95), red) == 0,
          "distant vertices do not create stray lines");
    viewer.remove_overlay("poly");
    check(count_colour(capture(), QRect(0, 0, 320, 240), red) == 0,
          "remove_overlay removes polygons");
    viewer.fit_all();

    viewer.show_shared(image.clone(), {box});
    viewer.remove_frame(~fv::FrameId{0});
    check(count_colour(capture(), QRect(0, 0, 320, 240), red) == 0,
          "removing a frame removes its attached boxes");

    // Both images stay immutable while the producer and painter hold references to them.
    const cv::Mat dark(240, 320, CV_8UC3, cv::Scalar::all(30));
    const cv::Mat light(240, 320, CV_8UC3, cv::Scalar::all(90));
    viewer.show_shared(dark, {box});
    std::atomic<bool> running{true};
    std::thread producer([&] {
        while (running) {
            viewer.show_shared(light, {green_box});
            viewer.show_shared(dark, {box});
            std::this_thread::yield();
        }
    });
    bool matched = true;
    for (int i = 0; i < 100; ++i) {
        shown = capture();
        const QColor background = shown.pixelColor(200, 200);
        const QColor outline = shown.pixelColor(100, 70);
        if (!((background == QColor(30, 30, 30) && outline == red) ||
              (background == QColor(90, 90, 90) && outline == green)))
            matched = false;
    }
    running = false;
    producer.join();
    check(matched, "rapid submissions never mix an image with another submission's boxes");

    // Rotated frames: a 100x100 square rotated 45 deg about (160, 120) is a diamond reaching
    // ~70.7 px from its centre, over a full-window frame that stays visible around it.
    const QColor under(200, 0, 0), over(0, 0, 200), canvas_grey(32, 32, 32);
    const cv::Mat under_image(240, 320, CV_8UC3, cv::Scalar(0, 0, 200));
    const cv::Mat over_image(100, 100, CV_8UC3, cv::Scalar(200, 0, 0));
    const cv::Rect2d over_rect(110, 70, 100, 100);
    viewer.clear_frames();
    viewer.set_frame(1, under_image, cv::Rect2d(0, 0, 320, 240));
    viewer.set_frame(2, over_image, over_rect, 1);
    viewer.show_world_rect(cv::Rect2d(0, 0, 320, 240));
    shown = capture();
    check(shown.pixelColor(110, 70) == over && shown.pixelColor(209, 169) == over &&
              shown.pixelColor(109, 69) == under && shown.pixelColor(210, 170) == under,
          "unrotated frame fills exactly its rect");
    viewer.set_frame(2, over_image, over_rect, 1, 45.0);
    shown = capture();
    check(shown.pixelColor(160, 120) == over, "rotated frame covers its centre");
    check(shown.pixelColor(160, 55) == over, "rotated frame reaches past its unrotated rect");
    check(shown.pixelColor(112, 72) == under, "frame below shows around a rotated frame");
    check(count_colour(shown, QRect(0, 0, 320, 240), QColor(0, 0, 0)) == 0,
          "rotated frame has no black background");
    const int over_area = count_colour(shown, QRect(0, 0, 320, 240), over);
    std::printf("rotated frame area: %d px (100x100 expected)\n", over_area);
    check(std::abs(over_area - 10000) <= 100, "rotated frame covers its own area, within 1%");
    viewer.set_frame(2, over_image, over_rect, 1, 90.0);
    shown = capture();
    check(count_colour(shown, QRect(0, 0, 320, 240), over) == 10000 &&
              shown.pixelColor(110, 70) == over && shown.pixelColor(209, 169) == over &&
              shown.pixelColor(109, 69) == under,
          "90 deg rotation of a square covers exactly its rect");
    viewer.set_frame(2, over_image, over_rect, 1, 45.0);
    viewer.move_frame(2, over_rect + cv::Point2d(20, 0));
    shown = capture();
    check(shown.pixelColor(180, 55) == over && shown.pixelColor(132, 72) == under,
          "move_frame keeps the rotation when no angle is given");
    viewer.move_frame(2, over_rect, {}, 0.0);
    check(capture().pixelColor(112, 72) == over, "move_frame can reset the rotation");

    viewer.remove_frame(1);
    viewer.set_frame(2, over_image, over_rect, 1, 45.0);
    viewer.fit_all();
    shown = capture();
    check(shown.pixelColor(45, 120) == over && shown.pixelColor(35, 120) == canvas_grey,
          "fit_all fits the rotated corners");

    const cv::Mat big_grey(1000, 1000, CV_8UC1, cv::Scalar(120));
    viewer.set_frame(3, big_grey, over_rect, 2, 30.0);
    viewer.show_world_rect(cv::Rect2d(0, 0, 320, 240));
    check(capture().pixelColor(160, 120) == QColor(120, 120, 120),
          "shrunk single-channel rotated frame is drawn from cached levels");
    viewer.set_frame_shared(3, big_grey.clone(), over_rect, 2, 30.0);
    viewer.set_frame_shared(3, big_grey.clone(), over_rect, 2, 30.0);  // now live
    check(capture().pixelColor(160, 120) == QColor(120, 120, 120),
          "shrunk live rotated frame is drawn");
    viewer.show_world_rect(cv::Rect2d(150, 110, 20, 15));
    check(capture().pixelColor(160, 120) == QColor(120, 120, 120),
          "enlarged rotated frame is drawn");

    // stats(): block the UI thread so three submissions arrive before any paint. The first two
    // are replaced undrawn; the paint after unblocking shows the third, 50+ ms after it was sent.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // empty the 1 s window
    check(viewer.stats().submitted_per_s == 0, "stats forget submissions after a second");
    std::atomic<bool> ui_blocked{false}, unblock{false};
    fv::detail::UiThread::instance().post([&] {
        ui_blocked = true;
        while (!unblock)
            std::this_thread::yield();
    });
    while (!ui_blocked)
        std::this_thread::yield();
    for (int i = 0; i < 3; ++i)
        viewer.set_frame(4, image, cv::Rect2d(0, 0, 32, 24));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    unblock = true;
    capture();
    const fv::ViewerStats st = viewer.stats();
    check(st.submitted_per_s > 0 && st.drawn_per_s > 0, "stats count submissions and paints");
    check(st.replaced_per_s > 0, "stats count submissions replaced before being drawn");
    check(st.paint_ms_avg > 0 && st.paint_ms_avg <= st.paint_ms_max,
          "paint time avg is positive and at most the max");
    check(st.latency_ms_avg >= 50 && st.latency_ms_avg <= st.latency_ms_max,
          "latency runs from submission to the paint showing it");
    viewer.remove_frame(4);

    // Overlay text colour: only the line given red is drawn red; the default stays grey.
    viewer.clear_frames();
    const QRect text_area(0, 0, 320, 80);
    viewer.set_overlay_text("plain", "plain text");
    check(count_reddish(capture(), text_area) == 0, "default overlay text is not red");
    viewer.set_overlay_text("warning", "WARNING", Qt::TopLeftCorner, cv::Scalar(0, 0, 255));
    check(count_reddish(capture(), text_area) > 0, "overlay text is drawn in its BGR colour");
    viewer.remove_overlay("warning");
    check(count_reddish(capture(), text_area) == 0, "removing the coloured line removes the red");
    viewer.remove_overlay("plain");
    viewer.close();
    std::printf("test_frame_boxes: %d failure(s)\n", failures);
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
            if (window->windowTitle() != QStringLiteral("test_frame_boxes")) continue;
            image = window->grab().toImage().convertToFormat(QImage::Format_RGB32);
            break;
        }
    });
    return image;
}

static int count_colour(const QImage& image, const QRect& area, const QColor& colour) {
    int count = 0;
    const QRect clipped = area.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y)
        for (int x = clipped.left(); x <= clipped.right(); ++x)
            if (image.pixelColor(x, y) == colour) ++count;
    return count;
}

/** Count clearly red pixels, including antialiased text edges. */
static int count_reddish(const QImage& image, const QRect& area) {
    int count = 0;
    const QRect clipped = area.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.red() > 150 && c.green() < 80 && c.blue() < 80) ++count;
        }
    }
    return count;
}

static void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}
