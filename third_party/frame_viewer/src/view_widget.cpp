#include "view_widget.h"

#include "compositor.h"
#include "key_utils.h"
#include "ui_thread.h"

#include <QCloseEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QWheelEvent>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace fv::detail {

namespace {

constexpr double wheel_zoom_step = 1.25; // per wheel notch
constexpr double key_zoom_step = 1.25;
constexpr double min_zoom = 1e-6;
constexpr double max_zoom = 1e4;
constexpr qint64 ESC_CLOSE_INTERVAL_MS = 500; // max gap between the two Esc presses that close the window

QFont overlay_font() {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(10);
    return f;
}

// Draws lines of text in a translucent box at a window corner.
void draw_text_box(QPainter& p, const QStringList& lines, Qt::Corner corner, const QRect& area) {
    if (lines.isEmpty()) return;
    const QFontMetrics fm(p.font());
    const int pad = 6, margin = 8;
    int text_w = 0;
    for (const QString& l : lines) text_w = std::max(text_w, fm.horizontalAdvance(l));
    const int box_w = text_w + 2 * pad;
    const int box_h = static_cast<int>(lines.size()) * fm.height() + 2 * pad;

    const bool left = corner == Qt::TopLeftCorner || corner == Qt::BottomLeftCorner;
    const bool top = corner == Qt::TopLeftCorner || corner == Qt::TopRightCorner;
    const int x = left ? area.left() + margin : area.right() - margin - box_w;
    const int y = top ? area.top() + margin : area.bottom() - margin - box_h;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 160));
    p.drawRoundedRect(QRect(x, y, box_w, box_h), 4, 4);
    p.setPen(QColor(235, 235, 235));
    int ty = y + pad + fm.ascent();
    for (const QString& l : lines) {
        p.drawText(x + pad, ty, l);
        ty += fm.height();
    }
}

} // namespace

view_widget::view_widget(std::shared_ptr<ViewerState> state) : state_(std::move(state)) {
    setAttribute(Qt::WA_OpaquePaintEvent); // we paint every pixel
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
    ui_thread::instance().register_widget(this);

    // Keeps the FPS readout fresh even when frames stop arriving.
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(250);
    QObject::connect(fps_timer_, &QTimer::timeout, this, [this] {
        bool enabled;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            enabled = state_->fps_enabled;
        }
        if (enabled) update();
    });
    fps_timer_->start();

    // Repaints in full quality once live frames stop changing (see compose()).
    quality_timer_ = new QTimer(this);
    quality_timer_->setSingleShot(true);
    quality_timer_->setInterval(600);
    QObject::connect(quality_timer_, &QTimer::timeout, this, [this] { update(); });
}

view_widget::~view_widget() {
    ui_thread::instance().unregister_widget(this);
}

void view_widget::wake_owner() {
    state_->events_cv.notify_all();
}

void view_widget::push_event(queued_event ev) {
    auto is_move = [](const queued_event& e) {
        return e.type == queued_event::kind::mouse && e.mouse.action == fv::MouseAction::MOVE;
    };
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        // Merge consecutive drag moves, so a slow owner gets the latest position, not a backlog
        if (is_move(ev) && !state_->events.empty() && is_move(state_->events.back()))
            state_->events.back() = std::move(ev);
        else
            state_->events.push_back(std::move(ev));
    }
    wake_owner();
}

// ---- Painting ------------------------------------------------------------------------------

void view_widget::paintEvent(QPaintEvent*) {
    const int w = width(), h = height();
    if (w <= 0 || h <= 0) return;
    if (canvas_.cols != w || canvas_.rows != h) canvas_.create(h, w, CV_8UC3);

    // Snapshot the frames (Mat headers only) so the lock is held for microseconds, not for the
    // whole paint. The snapshot keeps each image alive until this paint is done.
    std::vector<frame_entry> snapshot;
    std::uint64_t version;
    clock_type::time_point submit_time;
    {
        std::lock_guard<std::mutex> frames_lock(state_->frames_mutex);
        snapshot.reserve(state_->frames.size());
        for (const auto& [id, e] : state_->frames) snapshot.push_back(e);
        version = state_->frames_version;
        submit_time = state_->latest_submit_time;
    }

    bool viewport_queued = false;
    camera cam;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->auto_fit) {
            const cv::Rect2d bounds = content_bounds(snapshot);
            if (bounds.width > 0 && bounds.height > 0) {
                fit_camera(state_->cam, bounds);
                viewport_queued = state_->queue_viewport_locked();
            }
        }
        cam = state_->cam;
    }
    if (viewport_queued) wake_owner();

    const bool used_fast = compose(canvas_, snapshot, cam, clock_type::now());
    snapshot.clear(); // release our references to the images as early as possible

    // Live frames were shrunk with the fast method; repaint in full quality once they stop.
    if (used_fast) quality_timer_->start();

    // Qt 5.12 has no BGR888 format. BGRA bytes are Format_RGB32 on little-endian, which is also
    // the format Qt paints fastest (it converts any other format to it while drawing).
    cv::cvtColor(canvas_, m_canvas_bgra, cv::COLOR_BGR2BGRA);
    QPainter painter(this);
    const QImage image(m_canvas_bgra.data, w, h, static_cast<int>(m_canvas_bgra.step), QImage::Format_RGB32);
    painter.drawImage(0, 0, image);

    if (version != last_painted_version_) {
        last_painted_version_ = version;
        const auto now = clock_type::now();
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->shown_times.push_back(now);
        if (submit_time != clock_type::time_point{})
            state_->latencies_ms.emplace_back(now, std::chrono::duration<double, std::milli>(now - submit_time).count());
        while (state_->shown_times.size() > 1000) state_->shown_times.pop_front();
        while (state_->latencies_ms.size() > 1000) state_->latencies_ms.pop_front();
    }

    painter.setFont(overlay_font());
    draw_overlays(painter, cam);
    if (help_visible_) draw_help(painter);
}

void view_widget::draw_overlays(QPainter& painter, const camera& cam) {
    struct line_set { QStringList lines[4]; };
    auto corner_index = [](Qt::Corner c) { return static_cast<int>(c); };
    line_set sets;
    std::vector<overlay_rect> rects;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);

        for (const auto& [id, r] : state_->overlay_rects) rects.push_back(r);

        if (state_->fps_enabled) {
            // Rate over the last second from a list of timestamps.
            const auto now = clock_type::now();
            auto rate = [&](std::deque<clock_type::time_point>& times) {
                while (!times.empty() && now - times.front() > std::chrono::seconds(1)) times.pop_front();
                if (times.size() < 2) return static_cast<double>(times.size());
                const double span = std::chrono::duration<double>(times.back() - times.front()).count();
                return span > 0 ? (times.size() - 1) / span : 0.0;
            };
            const double in_fps = rate(state_->frame_times);
            const double shown_fps = rate(state_->shown_times);
            auto& lat = state_->latencies_ms;
            while (!lat.empty() && now - lat.front().first > std::chrono::seconds(1)) lat.pop_front();
            double lat_sum = 0;
            for (const auto& l : lat) lat_sum += l.second;

            sets.lines[corner_index(Qt::TopRightCorner)]
                << QString("FPS %1  shown %2").arg(in_fps, 0, 'f', 1).arg(shown_fps, 0, 'f', 1);
            if (!lat.empty())
                sets.lines[corner_index(Qt::TopRightCorner)]
                    << QString("latency %1 ms").arg(lat_sum / lat.size(), 0, 'f', 1);
        }

        std::vector<const overlay_item*> items;
        for (const auto& [id, item] : state_->overlays) items.push_back(&item);
        std::sort(items.begin(), items.end(),
                  [](const overlay_item* a, const overlay_item* b) { return a->order < b->order; });
        for (const overlay_item* item : items) {
            const QStringList lines = QString::fromStdString(item->text).split('\n');
            sets.lines[corner_index(item->corner)] << lines;
        }
    }

    // Boxes: world rect -> screen with the same camera as the image, so they stay aligned with
    // it; the pen width is in screen pixels, so it doesn't change with zoom.
    std::sort(rects.begin(), rects.end(),
              [](const overlay_rect& a, const overlay_rect& b) { return a.order < b.order; });
    for (const overlay_rect& r : rects) {
        const cv::Point2d tl = cam.to_screen(r.world_rect.tl());
        const cv::Point2d br = cam.to_screen(r.world_rect.br());
        // Clip to just outside the window: at high zoom the corners can be far off-screen,
        // where QPainter's coordinates lose precision. The clipped edges stay off-screen.
        const double pad = r.thickness_px + 2.0;
        const QRectF box = QRectF(QPointF(tl.x, tl.y), QPointF(br.x, br.y))
                               .intersected(QRectF(rect()).adjusted(-pad, -pad, pad, pad));
        if (box.isEmpty()) continue;

        const QColor colour(cv::saturate_cast<int>(r.colour[2]), cv::saturate_cast<int>(r.colour[1]),
                            cv::saturate_cast<int>(r.colour[0]));
        if (r.fill_opacity > 0) painter.fillRect(box, QColor(colour.red(), colour.green(), colour.blue(),
                                                             static_cast<int>(r.fill_opacity * 255)));
        if (r.thickness_px <= 0) continue;
        QPen pen(colour, r.thickness_px);
        pen.setJoinStyle(Qt::MiterJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
    }

    const QRect area = rect();
    for (Qt::Corner c : {Qt::TopLeftCorner, Qt::TopRightCorner, Qt::BottomLeftCorner, Qt::BottomRightCorner})
        draw_text_box(painter, sets.lines[corner_index(c)], c, area);
}

void view_widget::draw_help(QPainter& painter) {
    std::vector<std::pair<QString, QString>> rows;
    std::vector<binding> user_bindings;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        user_bindings = state_->bindings;
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
    const int extra_lines = 2 + (has_user ? 1 : 0); // title, spacer, "App" header
    const int box_w = key_w + gap + desc_w + 2 * pad;
    const int box_h = (static_cast<int>(rows.size()) + extra_lines) * line_h + (has_user ? section_gap : 0) + 2 * pad;
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

void view_widget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    bool queued;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->cam.screen_size = cv::Size(width(), height());
        queued = state_->queue_viewport_locked(); // (if auto-fitting, the next paint refits)
    }
    if (queued) wake_owner();
}

void view_widget::zoom_at(QPointF screen_pos, double factor) {
    bool queued;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        camera& c = state_->cam;
        const cv::Point2d anchor = c.to_world({screen_pos.x(), screen_pos.y()});
        c.zoom = std::clamp(c.zoom * factor, min_zoom, max_zoom);
        c.offset = {anchor.x - screen_pos.x() / c.zoom, anchor.y - screen_pos.y() / c.zoom};
        state_->auto_fit = false;
        queued = state_->queue_viewport_locked();
    }
    if (queued) wake_owner();
    update();
}

void view_widget::fit_all() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->auto_fit = true;
    }
    update(); // paint does the fit and reports the new viewport
}

// ---- Input ---------------------------------------------------------------------------------

void view_widget::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0; // 1 notch = 120
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QPointF pos = event->position();
#else
    const QPointF pos = event->posF(); // deprecated in 5.15, where position() replaces it
#endif
    if (steps != 0.0) zoom_at(pos, std::pow(wheel_zoom_step, steps));
    event->accept();
}

void view_widget::report_mouse(fv::MouseAction action, QMouseEvent* event) {
    queued_event ev;
    ev.type = queued_event::kind::mouse;
    ev.mouse.action = action;
    ev.mouse.button = event->button();
    ev.mouse.buttons = event->buttons();
    ev.mouse.modifiers = event->modifiers();
    ev.mouse.screen_pos = {event->localPos().x(), event->localPos().y()};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ev.mouse.world_pos = state_->cam.to_world(ev.mouse.screen_pos);
    }
    push_event(std::move(ev));
}

void view_widget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        panning_ = true;
        last_pan_pos_ = event->localPos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    report_mouse(fv::MouseAction::PRESS, event);
}

void view_widget::mouseMoveEvent(QMouseEvent* event) {
    if (!panning_) {
        // Drags only: mouse tracking is off, so Qt only sends moves while a button is held
        if (event->buttons() != Qt::NoButton) report_mouse(fv::MouseAction::MOVE, event);
        return;
    }
    const QPointF delta = event->localPos() - last_pan_pos_;
    last_pan_pos_ = event->localPos();
    bool queued;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        camera& c = state_->cam;
        c.offset.x -= delta.x() / c.zoom;
        c.offset.y -= delta.y() / c.zoom;
        state_->auto_fit = false;
        queued = state_->queue_viewport_locked();
    }
    if (queued) wake_owner();
    update();
}

void view_widget::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_ && event->button() == Qt::LeftButton) {
        panning_ = false;
        if (event->modifiers() & Qt::ControlModifier) setCursor(Qt::OpenHandCursor);
        else unsetCursor();
        return;
    }
    report_mouse(fv::MouseAction::RELEASE, event);
}

void view_widget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) return;
    report_mouse(fv::MouseAction::DOUBLE_CLICK, event);
}

bool view_widget::handle_builtin_key(Key k, bool auto_repeat) {
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
        help_visible_ = !help_visible_;
        update();
        return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoom_at(centre, key_zoom_step);
        return true;
    case Qt::Key_Minus:
        zoom_at(centre, 1.0 / key_zoom_step);
        return true;
    case Qt::Key_0:
        fit_all();
        return true;
    default:
        return false;
    }
}

void view_widget::keyReleaseEvent(QKeyEvent* event) {
    // Keep the closed-hand cursor until an in-progress pan ends (see mouseReleaseEvent)
    if (event->key() == Qt::Key_Control && !panning_) unsetCursor();
}

void view_widget::focusOutEvent(QFocusEvent* event) {
    // Ctrl's release is not delivered once focus is lost
    if (!panning_) unsetCursor();
    QWidget::focusOutEvent(event);
}

void view_widget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Control && !panning_) setCursor(Qt::OpenHandCursor);

    const auto code = static_cast<Qt::Key>(event->key());
    if (code == 0 || code == Qt::Key_unknown || is_modifier_only(code)) return;

    const Key k = normalize_key(Key(event->modifiers(), code));
    bool bound;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        bound = state_->is_bound_locked(k);
    }
    if (!bound && handle_builtin_key(k, event->isAutoRepeat())) return;

    queued_event ev;
    ev.type = queued_event::kind::key;
    ev.key = k;
    push_event(std::move(ev));
}

void view_widget::closeEvent(QCloseEvent* event) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true;
    }
    wake_owner();
    help_visible_ = false;
    event->accept(); // hides the window; it is deleted when the fv::Viewer is destroyed
}

} // namespace fv::detail
