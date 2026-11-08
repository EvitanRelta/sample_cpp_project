// ui_thread.h — the hidden thread that owns the QApplication and every viewer window.
#pragma once

#include <functional>
#include <mutex>
#include <set>
#include <thread>

class QApplication;
class QWidget;

namespace fv::detail {

class ui_thread {
public:
    // Starts the UI thread on first call; it is stopped when the program exits.
    static ui_thread& instance();

    // Runs fn on the UI thread (asynchronously, in posting order). Any thread.
    void post(std::function<void()> fn);

    // Runs fn on the UI thread and waits for it to finish. Any thread (runs inline on the UI thread).
    void post_and_wait(std::function<void()> fn);

    bool on_ui_thread() const { return std::this_thread::get_id() == thread_.get_id(); }

    // Viewer bookkeeping. When the last viewer goes away, an exit hook is registered so the UI
    // thread is shut down at program exit *before* Qt's own global objects are destroyed.
    void viewer_created();
    void viewer_destroyed();

    // Stops the event loop and joins the thread. Idempotent.
    void shutdown();

    // UI thread only: track windows so any left over are deleted on the UI thread at exit.
    void register_widget(QWidget* w) { widgets_.insert(w); }
    void unregister_widget(QWidget* w) { widgets_.erase(w); }

    ui_thread(const ui_thread&) = delete;
    ui_thread& operator=(const ui_thread&) = delete;

private:
    ui_thread();
    ~ui_thread();
    void run(std::function<void()> on_ready);

    static void shutdown_at_exit();

    std::thread thread_;
    std::mutex live_mutex_;
    int live_viewers_ = 0;
    std::mutex mutex_;
    QApplication* app_ = nullptr; // guarded by mutex_
    std::set<QWidget*> widgets_;  // UI thread only
};

} // namespace fv::detail
