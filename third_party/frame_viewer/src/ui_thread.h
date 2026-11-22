/** Owns the hidden thread that runs the QApplication and every viewer window. */
#pragma once

#include <functional>
#include <mutex>
#include <set>
#include <thread>

class QApplication;
class QWidget;

namespace fv::detail {

class UiThread {
   public:
    /** Start the UI thread on first call; it is stopped when the program exits. */
    static UiThread& instance();

    /** Run fn on the UI thread (asynchronously, in posting order). Any thread. */
    void post(std::function<void()> fn);

    /**
     * Run fn on the UI thread and wait for it to finish. Any thread (runs inline on the UI
     * thread).
     */
    void post_and_wait(std::function<void()> fn);

    bool on_ui_thread() const { return std::this_thread::get_id() == m_thread.get_id(); }

    /**
     * Track live viewers. When the last viewer goes away, an exit hook is registered so the UI
     * thread is shut down at program exit *before* Qt's own global objects are destroyed.
     */
    void viewer_created();
    void viewer_destroyed();

    /** Stop the event loop and join the thread. Idempotent. */
    void shutdown();

    /** Track windows so any left over are deleted on the UI thread at exit. UI thread only. */
    void register_widget(QWidget* w) { m_widgets.insert(w); }
    void unregister_widget(QWidget* w) { m_widgets.erase(w); }

    UiThread(const UiThread&) = delete;
    UiThread& operator=(const UiThread&) = delete;

   private:
    UiThread();
    ~UiThread();
    void run(std::function<void()> on_ready);

    static void shutdown_at_exit();

    std::thread m_thread;
    std::mutex m_live_mutex;
    int m_live_viewers = 0;
    std::mutex m_mutex;
    QApplication* m_app = nullptr;  // guarded by m_mutex
    std::set<QWidget*> m_widgets;   // UI thread only
};

}  // namespace fv::detail
