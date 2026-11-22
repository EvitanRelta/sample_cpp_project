/**
 * The viewer window: handles input, camera and painting on the UI thread. No Q_OBJECT, so no moc
 * is needed.
 */
#pragma once

#include <QElapsedTimer>
#include <QWidget>
#include <memory>
#include "viewer_state.h"

class QTimer;

namespace fv::detail {

class ViewWidget : public QWidget {
   public:
    explicit ViewWidget(std::shared_ptr<ViewerState> state);
    virtual ~ViewWidget() override;

   protected:
    virtual void paintEvent(QPaintEvent* event) override;
    virtual void resizeEvent(QResizeEvent* event) override;
    virtual void wheelEvent(QWheelEvent* event) override;
    virtual void mousePressEvent(QMouseEvent* event) override;
    virtual void mouseMoveEvent(QMouseEvent* event) override;
    virtual void mouseReleaseEvent(QMouseEvent* event) override;
    virtual void mouseDoubleClickEvent(QMouseEvent* event) override;
    virtual void keyPressEvent(QKeyEvent* event) override;
    virtual void keyReleaseEvent(QKeyEvent* event) override;
    virtual void focusOutEvent(QFocusEvent* event) override;
    virtual void closeEvent(QCloseEvent* event) override;

   private:
    void zoom_at(QPointF screen_pos, double factor);
    void fit_all();
    /**
     * Run k's built-in action, if any, and return whether k was consumed. Auto-repeated Esc is
     * ignored.
     */
    bool handle_builtin_key(Key k, bool auto_repeat);
    void report_mouse(fv::MouseAction action, QMouseEvent* event);
    void push_event(QueuedEvent ev);
    void wake_owner();  // notify the owner thread's wait_key()

    /** A frame's boxes and the geometry to place them, so painting needn't hold the image. */
    struct FrameBoxes {
        cv::Size image_size;
        cv::Rect2d world_rect;
        std::shared_ptr<const std::vector<Box>> boxes;
    };
    void draw_frame_boxes(class QPainter& painter,
                          const std::vector<FrameBoxes>& frames,
                          const Camera& cam);
    /** Draw overlay boxes, polygons and corner texts (plus the stats readout) from a snapshot. */
    void draw_overlays(class QPainter& painter,
                       const Camera& cam,
                       std::vector<OverlayItem> texts,
                       std::vector<OverlayBox> boxes,
                       std::vector<OverlayPolygon> polygons,
                       const std::optional<fv::ViewerStats>& stats);
    void draw_help(class QPainter& painter);

    std::shared_ptr<ViewerState> m_state;
    cv::Mat m_canvas;
    cv::Mat m_canvas_bgra;  // m_canvas converted for QImage::Format_RGB32
    bool m_panning = false;
    QPointF m_last_pan_pos;
    bool m_help_visible = false;
    QTimer* m_fps_timer = nullptr;
    QTimer* m_quality_timer = nullptr;
    std::uint64_t m_last_painted_version = 0;
    QElapsedTimer
        m_esc_timer;  // started on the first Esc press; a second press within the interval closes
};

}  // namespace fv::detail
