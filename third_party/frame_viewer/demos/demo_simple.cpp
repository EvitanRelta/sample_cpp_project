// demo_simple — the imshow-style case: render a frame in a loop and show it.
//
// Try: mouse wheel to zoom (fine grid detail appears), Ctrl+drag to pan, Ctrl+0 to fit,
// Ctrl+H for help, Esc twice to quit. Resize or snap the window; the image keeps fitting until you zoom.
// Press R to change the frame resolution mid-stream: the viewer accepts frames of any size.
#include <fv/viewer.h>

#include <opencv2/imgproc.hpp>

#include <cmath>

namespace {

// Static background: colour gradient plus a fine grid that only shows up when you zoom in.
cv::Mat make_background(cv::Size size) {
    cv::Mat bg(size, CV_8UC3);
    for (int y = 0; y < size.height; ++y) {
        auto* row = bg.ptr<cv::Vec3b>(y);
        for (int x = 0; x < size.width; ++x)
            row[x] = cv::Vec3b(static_cast<uchar>(60 + 120 * x / size.width),
                               static_cast<uchar>(40 + 80 * y / size.height), 70);
    }
    for (int x = 0; x < size.width; x += 8) cv::line(bg, {x, 0}, {x, size.height - 1}, {90, 90, 110}, 1);
    for (int y = 0; y < size.height; y += 8) cv::line(bg, {0, y}, {size.width - 1, y}, {90, 90, 110}, 1);
    for (int y = 64; y < size.height; y += 128)
        for (int x = 16; x < size.width; x += 128)
            cv::putText(bg, cv::format("%d,%d", x, y), {x, y}, cv::FONT_HERSHEY_PLAIN, 0.6, {200, 200, 200}, 1,
                        cv::LINE_AA);
    return bg;
}

void draw_scene(cv::Mat& frame, const cv::Mat& background, int i) {
    background.copyTo(frame);
    const double t = i * 0.03;
    for (int k = 0; k < 5; ++k) {
        const cv::Point centre(static_cast<int>(frame.cols * (0.5 + 0.4 * std::sin(t * (1 + 0.3 * k) + k))),
                               static_cast<int>(frame.rows * (0.5 + 0.4 * std::cos(t * (0.7 + 0.2 * k) + 2 * k))));
        cv::circle(frame, centre, 40 + 10 * k, cv::Scalar(255 - 40 * k, 120 + 25 * k, 60 * k), -1, cv::LINE_AA);
    }
    cv::putText(frame, cv::format("frame %d", i), {30, 60}, cv::FONT_HERSHEY_SIMPLEX, 1.5, {255, 255, 255}, 2,
                cv::LINE_AA);
}

} // namespace

int main() {
    fv::Viewer viewer("demo_simple", {1280, 720});
    viewer.show_fps();
    viewer.set_overlay_text("hint", "Wheel: zoom   Ctrl+drag: pan   R: change resolution   Ctrl+H: help   Esc twice: quit",
                            Qt::BottomLeftCorner);

    const cv::Size sizes[] = {{1280, 720}, {640, 360}, {1920, 1080}, {720, 720}};
    int size_index = 0;
    cv::Mat background = make_background(sizes[size_index]);

    // Runs on this thread inside wait_key(), so changing `background` here is safe.
    viewer.bind(Qt::Key_R, "Change frame resolution", [&] {
        size_index = (size_index + 1) % 4;
        background = make_background(sizes[size_index]);
        viewer.fit_all(); // optional: refit, since the world size changed
    });

    cv::Mat frame;
    for (int i = 0; viewer.is_open(); ++i) {
        draw_scene(frame, background, i);
        viewer.show(frame); // copies, so reusing `frame` next iteration is safe
        viewer.set_overlay_text("size", cv::format("frame %d x %d", frame.cols, frame.rows));
        viewer.wait_key(15); // ~60 FPS; also runs the R binding when pressed
    }
}
