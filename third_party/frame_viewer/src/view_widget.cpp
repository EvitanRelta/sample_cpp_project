#include "view_widget.h"
#include <QCloseEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <utility>
#include <vector>
#include "compositor.h"
#include "key_utils.h"
#include "ui_thread.h"

namespace fv::detail {

static constexpr double WHEEL_ZOOM_STEP = 1.25;  // per wheel notch
static constexpr double KEY_ZOOM_STEP = 1.25;
static constexpr double MIN_ZOOM = 1e-6;
static constexpr double MAX_ZOOM = 1e4;
static constexpr qint64 ESC_CLOSE_INTERVAL_MS =
    500;  // max gap between the two Esc presses that close the window

using TextLine = std::pair<QString, QColor>;  // one line of corner text and its colour

// Forward declarations of helper functions
static QFont overlay_font();
static void draw_text_box(QPainter& p,
                          const std::vector<TextLine>& lines,
                          Qt::Corner corner,
                          const QRect& area);
static void draw_box(QPainter& painter,
                     const Box& box,
                     const QRectF& screen_box,
                     const QRectF& visible);

ViewWidget::ViewWidget(std::shared_ptr<ViewerState> state) : m_state(std::move(state)) {
    setAttribute(Qt::WA_OpaquePaintEvent);  // we paint every pixel
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
    UiThread::instance().register_widget(this);

    // Keeps the FPS readout fresh even when frames stop arriving.
    m_fps_timer = new QTimer(this);
    m_fps_timer->setInterval(250);
    QObject::connect(m_fps_timer, &QTimer::timeout, this, [this] {
        bool enabled;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            enabled = m_state->fps_enabled;
        }
        if (enabled) update();
    });
    m_fps_timer->start();

    // Repaints in full quality once live frames stop changing (see compose()).
    m_quality_timer = new QTimer(this);
    m_quality_timer->setSingleShot(true);
    m_quality_timer->setInterval(600);
    QObject::connect(m_quality_timer, &QTimer::timeout, this, [this] { update(); });
}

ViewWidget::~ViewWidget() {
    UiThread::instance().unregister_widget(this);
}

void ViewWidget::wake_owner() {
    m_state->events_cv.notify_all();
}

void ViewWidget::push_event(QueuedEvent ev) {
    auto is_move = [](const QueuedEvent& e) {
        return e.type == QueuedEvent::Kind::MOUSE && e.mouse.action == fv::MouseAction::MOVE;
    };
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        // Merge consecutive drag moves, so a slow owner gets the latest position, not a backlog
        if (is_move(ev) && !m_state->events.empty() && is_move(m_state->events.back()))
            m_state->events.back() = std::move(ev);
        else
            m_state->events.push_back(std::move(ev));
    }
    wake_owner();
}

// ---- Painting ------------------------------------------------------------------------------

void ViewWidget::paintEvent(QPaintEvent*) {
    const auto paint_start = ClockType::now();
    const int w = width(), h = height();
    if (w <= 0 || h <= 0) return;
    if (m_canvas.cols != w || m_canvas.rows != h) m_canvas.create(h, w, CV_8UC3);

    // Snapshot frames, their boxes, overlays and the camera within one lock hold, so a paint never
    // mixes versions (e.g. half of a batch()). Only Mat headers and box-list pointers are copied
    // for frames, not image or box data. The snapshot keeps each image alive until it is composed.
    std::vector<FrameEntry> snapshot;
    std::vector<ClockType::time_point> first_shown;  // submit times of images new to this paint
    std::uint64_t version;
    std::vector<OverlayItem> texts;
    std::vector<OverlayBox> boxes;
    std::vector<OverlayPolygon> polygons;
    std::optional<fv::ViewerStats> stats;  // set when the readout is shown
    bool viewport_queued = false;
    Camera cam;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        snapshot.reserve(m_state->frames.size());
        for (auto& [id, e] : m_state->frames) {
            if (!e.drawn) first_shown.push_back(e.last_update);
            e.drawn = true;
            snapshot.push_back(e);
        }
        version = m_state->frames_version;
        for (const auto& [id, t] : m_state->overlays)
            texts.push_back(t);
        for (const auto& [id, b] : m_state->overlay_boxes)
            boxes.push_back(b);
        for (const auto& [id, p] : m_state->overlay_polygons)
            polygons.push_back(p);
        if (m_state->fps_enabled) stats = m_state->stats_locked(ClockType::now());

        if (m_state->auto_fit) {
            const cv::Rect2d bounds = content_bounds(snapshot);
            if (bounds.width > 0 && bounds.height > 0) {
                fit_camera(m_state->cam, bounds);
                viewport_queued = m_state->queue_viewport_locked();
            }
        }
        cam = m_state->cam;
    }
    if (viewport_queued) wake_owner();

    const bool used_fast = compose(m_canvas, snapshot, cam, ClockType::now());
    // Keep only what box drawing needs, so the images are released before the rest of the paint.
    std::vector<FrameBoxes> boxed_frames;
    for (const auto& f : snapshot)
        if (f.boxes && !f.image.empty())
            boxed_frames.push_back({f.image.size(), f.world_rect, f.boxes});
    snapshot.clear();  // release our references to the images as early as possible

    // Live frames were shrunk with the fast method; repaint in full quality once they stop.
    if (used_fast) m_quality_timer->start();

    // Qt 5.12 has no BGR888 format. BGRA bytes are Format_RGB32 on little-endian, which is also
    // the format Qt paints fastest (it converts any other format to it while drawing).
    cv::cvtColor(m_canvas, m_canvas_bgra, cv::COLOR_BGR2BGRA);
    QPainter painter(this);
    const QImage image(m_canvas_bgra.data, w, h, static_cast<int>(m_canvas_bgra.step),
                       QImage::Format_RGB32);
    painter.drawImage(0, 0, image);

    painter.setFont(overlay_font());
    draw_frame_boxes(painter, boxed_frames, cam);
    draw_overlays(painter, cam, std::move(texts), std::move(boxes), std::move(polygons), stats);
    if (m_help_visible) draw_help(painter);

    const auto end = ClockType::now();
    auto ms = [](ClockType::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->paint_ms.add(end, ms(end - paint_start));
    for (ClockType::time_point submitted : first_shown)
        m_state->latency_ms.add(end, ms(end - submitted));
    if (version != m_last_painted_version) m_state->drawn_times.add(end);
    m_last_painted_version = version;
}

void ViewWidget::draw_frame_boxes(QPainter& painter,
                                  const std::vector<FrameBoxes>& frames,
                                  const Camera& cam) {
    for (const auto& frame : frames) {
        if (frame.world_rect.width <= 0 || frame.world_rect.height <= 0) continue;
        const cv::Size size = frame.image_size;
        const cv::Point2d origin = cam.to_screen(frame.world_rect.tl());
        const double scale_x = cam.zoom * frame.world_rect.width / size.width;
        const double scale_y = cam.zoom * frame.world_rect.height / size.height;
        const QRectF image_rect(origin.x, origin.y, size.width * scale_x, size.height * scale_y);
        const QRectF visible = image_rect.intersected(QRectF(rect()));
        if (visible.isEmpty()) continue;

        painter.save();
        painter.setClipRect(visible);
        for (const auto& box : *frame.boxes) {
            const QRectF screen_box(origin.x + box.rect.x * scale_x,
                                    origin.y + box.rect.y * scale_y, box.rect.width * scale_x,
                                    box.rect.height * scale_y);
            draw_box(painter, box, screen_box, visible);
        }
        painter.restore();
    }
}

void ViewWidget::draw_overlays(QPainter& painter,
                               const Camera& cam,
                               std::vector<OverlayItem> texts,
                               std::vector<OverlayBox> boxes,
                               std::vector<OverlayPolygon> polygons,
                               const std::optional<fv::ViewerStats>& stats) {
    auto corner_index = [](Qt::Corner c) { return static_cast<int>(c); };
    std::vector<TextLine> corner_lines[4];
    if (stats) {
        const QColor grey(235, 235, 235);
        auto& lines = corner_lines[corner_index(Qt::TopRightCorner)];
        lines.emplace_back(QString("submitted %1  drawn %2  replaced %3")
                               .arg(stats->submitted_per_s, 0, 'f', 1)
                               .arg(stats->drawn_per_s, 0, 'f', 1)
                               .arg(stats->replaced_per_s, 0, 'f', 1),
                           grey);
        lines.emplace_back(QString("paint %1/%2 ms  latency %3/%4 ms")
                               .arg(stats->paint_ms_avg, 0, 'f', 1)
                               .arg(stats->paint_ms_max, 0, 'f', 1)
                               .arg(stats->latency_ms_avg, 0, 'f', 1)
                               .arg(stats->latency_ms_max, 0, 'f', 1),
                           grey);
    }
    std::sort(texts.begin(), texts.end(),
              [](const OverlayItem& a, const OverlayItem& b) { return a.order < b.order; });
    for (const OverlayItem& item : texts) {
        const QColor colour(cv::saturate_cast<uchar>(item.colour[2]),
                            cv::saturate_cast<uchar>(item.colour[1]),
                            cv::saturate_cast<uchar>(item.colour[0]));
        for (const QString& line : QString::fromStdString(item.text).split('\n'))
            corner_lines[corner_index(item.corner)].emplace_back(line, colour);
    }

    // Boxes: world rect -> screen with the same camera as the image, so they stay aligned with
    // it; the pen width and labels are in screen pixels, so they don't change with zoom.
    std::sort(boxes.begin(), boxes.end(),
              [](const OverlayBox& a, const OverlayBox& b) { return a.order < b.order; });
    for (const OverlayBox& b : boxes) {
        const cv::Point2d tl = cam.to_screen(b.box.rect.tl());
        const cv::Point2d br = cam.to_screen(b.box.rect.br());
        draw_box(painter, b.box, QRectF(QPointF(tl.x, tl.y), QPointF(br.x, br.y)), QRectF(rect()));
    }

    // Polygons: same camera and pen rules as boxes. Edges and fills are clipped to just outside
    // the window in double precision first, for the same QPainter precision reason.
    std::sort(polygons.begin(), polygons.end(),
              [](const OverlayPolygon& a, const OverlayPolygon& b) { return a.order < b.order; });
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    for (const OverlayPolygon& poly : polygons) {
        const int count = static_cast<int>(poly.world_pts.size());
        if (count == 0) continue;
        const double pad = poly.thickness_px + 2.0;
        const QRectF bounds = QRectF(rect()).adjusted(-pad, -pad, pad, pad);
        QPolygonF screen_pts;
        for (const cv::Point2d& w : poly.world_pts) {
            const cv::Point2d s = cam.to_screen(w);
            screen_pts << QPointF(s.x, s.y);
        }
        const bool finite = std::all_of(screen_pts.begin(), screen_pts.end(), [](QPointF s) {
            return std::isfinite(s.x()) && std::isfinite(s.y());
        });
        if (!finite) continue;
        const QColor colour(cv::saturate_cast<uchar>(poly.colour[2]),
                            cv::saturate_cast<uchar>(poly.colour[1]),
                            cv::saturate_cast<uchar>(poly.colour[0]));

        if (poly.closed && poly.fill_opacity > 0 && count >= 3) {
            QPainterPath shape, window;
            shape.addPolygon(screen_pts);
            window.addRect(bounds);
            painter.fillPath(shape.intersected(window),
                             QColor(colour.red(), colour.green(), colour.blue(),
                                    static_cast<int>(poly.fill_opacity * 255)));
        }
        if (poly.thickness_px <= 0) continue;
        // Round caps and joins, so edges split by clipping still meet cleanly
        painter.setPen(QPen(colour, poly.thickness_px, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (count == 1) {
            if (bounds.contains(screen_pts[0])) painter.drawPoint(screen_pts[0]);
            continue;
        }

        QPainterPath outline;
        const int edge_count = poly.closed ? count : count - 1;
        for (int i = 0; i < edge_count; ++i) {
            const QPointF a = screen_pts[i];
            const QPointF d = screen_pts[(i + 1) % count] - a;
            // Liang-Barsky: keep the part a + t * d, t in [t0, t1], that lies inside bounds
            const double p[4] = {-d.x(), d.x(), -d.y(), d.y()};
            const double q[4] = {a.x() - bounds.left(), bounds.right() - a.x(),
                                 a.y() - bounds.top(), bounds.bottom() - a.y()};
            double t0 = 0.0, t1 = 1.0;
            for (int k = 0; k < 4; ++k) {
                if (p[k] == 0) {
                    if (q[k] < 0) t0 = 2.0;  // parallel to this side and outside it
                    continue;
                }
                const double t = q[k] / p[k];
                if (p[k] < 0)
                    t0 = std::max(t0, t);
                else
                    t1 = std::min(t1, t);
            }
            if (t0 > t1) continue;

            const QPointF start = a + t0 * d;
            // Unclipped edges continue the previous one, so their joins stay joins
            if (outline.elementCount() == 0 || outline.currentPosition() != start)
                outline.moveTo(start);
            outline.lineTo(a + t1 * d);
        }
        painter.drawPath(outline);
    }
    painter.restore();

    const QRect area = rect();
    for (Qt::Corner c :
         {Qt::TopLeftCorner, Qt::TopRightCorner, Qt::BottomLeftCorner, Qt::BottomRightCorner})
        draw_text_box(painter, corner_lines[corner_index(c)], c, area);
}

void ViewWidget::draw_help(QPainter& painter) {
    std::vector<std::pair<QString, QString>> rows;
    std::vector<Binding> user_bindings;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        user_bindings = m_state->bindings;
    }
    auto overridden = [&](Key k) {
        for (const auto& b : user_bindings)
            if (b.key == k) return true;
        return false;
    };

    rows.push_back({"Mouse wheel", "Zoom at cursor"});
    rows.push_back({"Ctrl + left click drag", "Pan"});
    if (!overridden(Qt::CTRL | Qt::Key_Plus)) rows.push_back({"Ctrl + [+/-]", "Zoom in / out"});
    if (!overridden(Qt::CTRL | Qt::Key_0)) rows.push_back({"Ctrl + 0", "Fit frame"});
    if (!overridden(Qt::CTRL | Qt::Key_H)) rows.push_back({"Ctrl + H", "Toggle this help"});
    if (!overridden(Qt::Key_Escape)) rows.push_back({"Esc twice", "Close window"});
    const size_t builtin_count = rows.size();
    for (const auto& b : user_bindings)
        rows.push_back({key_name(b.key), QString::fromStdString(b.description)});

    const QFontMetrics fm(painter.font());
    int key_w = 0, desc_w = fm.horizontalAdvance("Controls");
    for (const auto& [k, d] : rows) {
        key_w = std::max(key_w, fm.horizontalAdvance(k));
        desc_w = std::max(desc_w, fm.horizontalAdvance(d));
    }
    const int pad = 14, gap = 24, line_h = fm.height() + 2;
    const bool has_user = rows.size() > builtin_count;
    const int section_gap = line_h / 2;
    const int extra_lines = 2 + (has_user ? 1 : 0);  // title, spacer, "App" header
    const int box_w = key_w + gap + desc_w + 2 * pad;
    const int box_h = (static_cast<int>(rows.size()) + extra_lines) * line_h +
                      (has_user ? section_gap : 0) + 2 * pad;
    const QRect box((width() - box_w) / 2, (height() - box_h) / 2, box_w, box_h);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 200));
    painter.drawRoundedRect(box, 8, 8);

    int y = box.top() + pad + fm.ascent();
    const int kx = box.left() + pad, dx = kx + key_w + gap;
    painter.setPen(QColor(255, 255, 255));
    QFont bold = painter.font();
    bold.setBold(true);
    const QFont normal = painter.font();
    painter.setFont(bold);
    painter.drawText(kx, y, "Controls");
    y += 2 * line_h;

    for (size_t i = 0; i < rows.size(); ++i) {
        if (i == builtin_count && has_user) {
            y += section_gap;
            painter.setFont(bold);
            painter.setPen(QColor(255, 255, 255));
            painter.drawText(kx, y, "App");
            y += line_h;
        }
        painter.setFont(normal);
        painter.setPen(QColor(150, 200, 255));
        painter.drawText(kx, y, rows[i].first);
        painter.setPen(QColor(235, 235, 235));
        painter.drawText(dx, y, rows[i].second);
        y += line_h;
    }
}

// ---- Camera --------------------------------------------------------------------------------

void ViewWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    bool queued;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->cam.screen_size = cv::Size(width(), height());
        queued = m_state->queue_viewport_locked();  // (if auto-fitting, the next paint refits)
    }
    if (queued) wake_owner();
}

void ViewWidget::zoom_at(QPointF screen_pos, double factor) {
    bool queued;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        Camera& c = m_state->cam;
        const cv::Point2d anchor = c.to_world({screen_pos.x(), screen_pos.y()});
        c.zoom = std::clamp(c.zoom * factor, MIN_ZOOM, MAX_ZOOM);
        c.offset = {anchor.x - screen_pos.x() / c.zoom, anchor.y - screen_pos.y() / c.zoom};
        m_state->auto_fit = false;
        queued = m_state->queue_viewport_locked();
    }
    if (queued) wake_owner();
    update();
}

void ViewWidget::fit_all() {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->auto_fit = true;
    }
    update();  // paint does the fit and reports the new viewport
}

// ---- Input ---------------------------------------------------------------------------------

void ViewWidget::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;  // 1 notch = 120
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QPointF pos = event->position();
#else
    const QPointF pos = event->posF();  // deprecated in 5.15, where position() replaces it
#endif
    if (steps != 0.0) zoom_at(pos, std::pow(WHEEL_ZOOM_STEP, steps));
    event->accept();
}

void ViewWidget::report_mouse(fv::MouseAction action, QMouseEvent* event) {
    QueuedEvent ev;
    ev.type = QueuedEvent::Kind::MOUSE;
    ev.mouse.action = action;
    ev.mouse.button = event->button();
    ev.mouse.buttons = event->buttons();
    ev.mouse.modifiers = event->modifiers();
    ev.mouse.screen_pos = {event->localPos().x(), event->localPos().y()};
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        ev.mouse.world_pos = m_state->cam.to_world(ev.mouse.screen_pos);
    }
    push_event(std::move(ev));
}

void ViewWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        m_panning = true;
        m_last_pan_pos = event->localPos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    report_mouse(fv::MouseAction::PRESS, event);
}

void ViewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_panning) {
        // Drags only: mouse tracking is off, so Qt only sends moves while a button is held
        if (event->buttons() != Qt::NoButton) report_mouse(fv::MouseAction::MOVE, event);
        return;
    }
    const QPointF delta = event->localPos() - m_last_pan_pos;
    m_last_pan_pos = event->localPos();
    bool queued;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        Camera& c = m_state->cam;
        c.offset.x -= delta.x() / c.zoom;
        c.offset.y -= delta.y() / c.zoom;
        m_state->auto_fit = false;
        queued = m_state->queue_viewport_locked();
    }
    if (queued) wake_owner();
    update();
}

void ViewWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (m_panning && event->button() == Qt::LeftButton) {
        m_panning = false;
        if (event->modifiers() & Qt::ControlModifier)
            setCursor(Qt::OpenHandCursor);
        else
            unsetCursor();
        return;
    }
    report_mouse(fv::MouseAction::RELEASE, event);
}

void ViewWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) return;
    report_mouse(fv::MouseAction::DOUBLE_CLICK, event);
}

bool ViewWidget::handle_builtin_key(Key k, bool auto_repeat) {
    if (k == Qt::Key_Escape) {
        // Close only on two distinct presses in quick succession; holding Esc does not count
        if (auto_repeat) return true;
        if (m_esc_timer.isValid() && m_esc_timer.elapsed() <= ESC_CLOSE_INTERVAL_MS) {
            m_esc_timer.invalidate();
            close();
        } else {
            m_esc_timer.start();
        }
        return true;
    }
    if (k.modifiers() != Qt::ControlModifier) return false;

    const QPointF centre(width() / 2.0, height() / 2.0);
    switch (k.code()) {
        case Qt::Key_H:
            m_help_visible = !m_help_visible;
            update();
            return true;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            zoom_at(centre, KEY_ZOOM_STEP);
            return true;
        case Qt::Key_Minus:
            zoom_at(centre, 1.0 / KEY_ZOOM_STEP);
            return true;
        case Qt::Key_0:
            fit_all();
            return true;
        default:
            return false;
    }
}

void ViewWidget::keyReleaseEvent(QKeyEvent* event) {
    // Keep the closed-hand cursor until an in-progress pan ends (see mouseReleaseEvent)
    if (event->key() == Qt::Key_Control && !m_panning) unsetCursor();
}

void ViewWidget::focusOutEvent(QFocusEvent* event) {
    // Ctrl's release is not delivered once focus is lost
    if (!m_panning) unsetCursor();
    QWidget::focusOutEvent(event);
}

void ViewWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Control && !m_panning) setCursor(Qt::OpenHandCursor);

    const auto code = static_cast<Qt::Key>(event->key());
    if (code == 0 || code == Qt::Key_unknown || is_modifier_only(code)) return;

    const Key k = normalize_key(Key(event->modifiers(), code));
    bool bound;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        bound = m_state->is_bound_locked(k);
    }
    if (!bound && handle_builtin_key(k, event->isAutoRepeat())) return;

    QueuedEvent ev;
    ev.type = QueuedEvent::Kind::KEY;
    ev.key = k;
    push_event(std::move(ev));
}

void ViewWidget::closeEvent(QCloseEvent* event) {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->closed = true;
    }
    wake_owner();
    m_help_visible = false;
    event->accept();  // hides the window; it is deleted when the fv::Viewer is destroyed
}

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
static QFont overlay_font() {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(10);
    return f;
}

/** Draw lines of text, each in its own colour, in a translucent box at a window corner. */
static void draw_text_box(QPainter& p,
                          const std::vector<TextLine>& lines,
                          Qt::Corner corner,
                          const QRect& area) {
    if (lines.empty()) return;
    const QFontMetrics fm(p.font());
    const int pad = 6, margin = 8;
    int text_w = 0;
    for (const auto& [text, colour] : lines)
        text_w = std::max(text_w, fm.horizontalAdvance(text));
    const int box_w = text_w + 2 * pad;
    const int box_h = static_cast<int>(lines.size()) * fm.height() + 2 * pad;

    const bool left = corner == Qt::TopLeftCorner || corner == Qt::BottomLeftCorner;
    const bool top = corner == Qt::TopLeftCorner || corner == Qt::TopRightCorner;
    const int x = left ? area.left() + margin : area.right() - margin - box_w;
    const int y = top ? area.top() + margin : area.bottom() - margin - box_h;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 160));
    p.drawRoundedRect(QRect(x, y, box_w, box_h), 4, 4);
    int ty = y + pad + fm.ascent();
    for (const auto& [text, colour] : lines) {
        p.setPen(colour);
        p.drawText(x + pad, ty, text);
        ty += fm.height();
    }
}

/**
 * Draw a box's fill, outline and label at `screen_box`, keeping them inside `visible` (the visible
 * image for frame-attached boxes, the window for overlays).
 */
static void draw_box(QPainter& painter,
                     const Box& box,
                     const QRectF& screen_box,
                     const QRectF& visible) {
    if (!std::isfinite(screen_box.left()) || !std::isfinite(screen_box.top()) ||
        !std::isfinite(screen_box.right()) || !std::isfinite(screen_box.bottom()) ||
        screen_box.isEmpty())
        return;
    // Clip to just outside `visible`: at high zoom the corners can be far off-screen, where
    // QPainter's coordinates lose precision. The clipped edges stay outside `visible`.
    const double pad = std::max(box.thickness_px, 0) + 2.0;
    const QRectF clipped = screen_box.intersected(visible.adjusted(-pad, -pad, pad, pad));
    if (clipped.isEmpty()) return;

    const QColor colour(cv::saturate_cast<uchar>(box.colour[2]),
                        cv::saturate_cast<uchar>(box.colour[1]),
                        cv::saturate_cast<uchar>(box.colour[0]));
    if (box.fill_opacity > 0) {
        const int alpha = static_cast<int>(std::min(box.fill_opacity, 1.0) * 255);
        painter.fillRect(clipped.intersected(visible),
                         QColor(colour.red(), colour.green(), colour.blue(), alpha));
    }
    if (box.thickness_px > 0) {
        QPen pen(colour, box.thickness_px);
        pen.setJoinStyle(Qt::MiterJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(clipped);
    }
    if (box.label.empty() || !screen_box.intersects(visible)) return;

    const QFontMetrics fm(painter.font());
    const int padding = 3;
    const int available_width = static_cast<int>(visible.width()) - 2 * padding;
    if (available_width <= 0) return;
    const QString label =
        fm.elidedText(QString::fromStdString(box.label), Qt::ElideRight, available_width);
    const double width = fm.horizontalAdvance(label) + 2 * padding;
    const double height = fm.height() + 2 * padding;
    const double x = std::clamp(screen_box.left(), visible.left(),
                                std::max(visible.left(), visible.right() - width));
    // Prefer above the box; at the top edge, keep the label inside `visible`.
    const double y = std::clamp(screen_box.top() - height - padding, visible.top(),
                                std::max(visible.top(), visible.bottom() - height));
    // Box-coloured tag, with black or white text, whichever contrasts with the colour.
    painter.fillRect(QRectF(x, y, width, height), colour);
    // WCAG 2 contrast ratios against black and white, from linear-light relative luminance
    // (not the encoded sRGB channel values).
    const auto linearize = [](int channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * linearize(colour.red()) + 0.7152 * linearize(colour.green()) +
                             0.0722 * linearize(colour.blue());
    const double black_contrast = (luminance + 0.05) / 0.05;
    const double white_contrast = 1.05 / (luminance + 0.05);
    painter.setPen(black_contrast >= white_contrast ? Qt::black : Qt::white);
    painter.drawText(QPointF(x + padding, y + padding + fm.ascent()), label);
}

}  // namespace fv::detail
