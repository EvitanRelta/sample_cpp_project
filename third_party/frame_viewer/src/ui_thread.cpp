#include "ui_thread.h"
#include <QApplication>
#include <QMetaObject>
#include <QWidget>
#include <cstdlib>
#include <future>
#include <memory>
#include <vector>

namespace fv::detail {

UiThread& UiThread::instance() {
    // Function-local static: created on first use, destroyed (thread joined) at program exit.
    static UiThread instance;
    return instance;
}

UiThread::UiThread() {
    auto ready = std::make_shared<std::promise<void>>();
    std::future<void> ready_future = ready->get_future();
    m_thread = std::thread([this, ready] { run([ready] { ready->set_value(); }); });
    ready_future.wait();
}

UiThread::~UiThread() {
    shutdown();  // fallback; normally already done by the exit hook
}

void UiThread::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_app)
            QMetaObject::invokeMethod(
                m_app, [] { QCoreApplication::exit(0); }, Qt::QueuedConnection);
    }
    if (m_thread.joinable() && !on_ui_thread()) m_thread.join();
}

void UiThread::shutdown_at_exit() {
    instance().shutdown();
}

void UiThread::viewer_created() {
    std::lock_guard<std::mutex> lock(m_live_mutex);
    ++m_live_viewers;
}

void UiThread::viewer_destroyed() {
    std::lock_guard<std::mutex> lock(m_live_mutex);
    if (--m_live_viewers == 0) {
        // Exit hooks run in reverse registration order. Registering now, after Qt has created
        // whatever global objects it needed for our windows, makes our shutdown run before
        // Qt destroys them. (Registering again later is harmless: shutdown() is idempotent.)
        std::atexit(&UiThread::shutdown_at_exit);
    }
}

void UiThread::run(std::function<void()> on_ready) {
    // QApplication must be the first QObject created in the process. Whichever thread
    // creates it becomes Qt's "main thread"; for us, that's this hidden thread.
    static int argc = 1;
    static char app_name[] = "frame_viewer";
    static char* argv[] = {app_name, nullptr};

    {
        QApplication app(argc, argv);
        QApplication::setQuitOnLastWindowClosed(false);  // windows come and go; we quit at exit

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_app = &app;
        }
        on_ready();

        QApplication::exec();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_app = nullptr;  // no more posts accepted
        }

        // Delete any windows still alive, on this thread, before QApplication goes away.
        std::vector<QWidget*> leftovers(m_widgets.begin(), m_widgets.end());
        for (QWidget* w : leftovers)
            delete w;
        m_widgets.clear();

        // Run deferred deletes and drop queued functors while the app still exists.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void UiThread::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_app) return;  // shutting down
    QMetaObject::invokeMethod(m_app, std::move(fn), Qt::QueuedConnection);
}

void UiThread::post_and_wait(std::function<void()> fn) {
    if (on_ui_thread()) {
        fn();
        return;
    }
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> done_future = done->get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_app) return;  // shutting down: nothing will run it
        QMetaObject::invokeMethod(
            m_app,
            [fn = std::move(fn), done] {
                fn();
                done->set_value();
            },
            Qt::QueuedConnection);
    }
    done_future.wait();
}

}  // namespace fv::detail
