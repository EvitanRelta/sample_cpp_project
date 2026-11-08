// view_widget.h — the window. Lives on the UI thread only. Handles input, camera and painting.
// No Q_OBJECT (no signals/slots), so no moc is needed.
#pragma once

#include "viewer_state.h"

#include <QElapsedTimer>
#include <QWidget>

#include <memory>

class QTimer;

namespace fv::detail {

class view_widget : public QWidget {
public:
    explicit view_widget(std::shared_ptr<ViewerState> state);
    ~view_widget() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void zoom_at(QPointF screen_pos, double factor);
    void fit_all();
    /** Run k's built-in action, if any, and return whether k was consumed. Auto-repeated Esc is ignored. */
    bool handle_builtin_key(Key k, bool auto_repeat);
    void report_mouse(fv::MouseAction action, QMouseEvent* event);
    void push_event(queued_event ev);
    void wake_owner(); // notify the owner thread's wait_key()
    void draw_overlays(class QPainter& painter, const camera& cam);
    void draw_help(class QPainter& painter);

    std::shared_ptr<ViewerState> state_;
    cv::Mat canvas_;
    cv::Mat m_canvas_bgra; // canvas_ converted for QImage::Format_RGB32
    bool panning_ = false;
    QPointF last_pan_pos_;
    bool help_visible_ = false;
    QTimer* fps_timer_ = nullptr;
    QTimer* quality_timer_ = nullptr;
    std::uint64_t last_painted_version_ = 0;
    QElapsedTimer m_esc_timer; // started on the first Esc press; a second press within the interval closes
};

} // namespace fv::detail
