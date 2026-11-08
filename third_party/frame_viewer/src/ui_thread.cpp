#include "ui_thread.h"

#include <QApplication>
#include <QMetaObject>
#include <QWidget>

#include <cstdlib>
#include <future>
#include <memory>
#include <vector>

namespace fv::detail {

ui_thread& ui_thread::instance() {
    // Function-local static: created on first use, destroyed (thread joined) at program exit.
    static ui_thread instance;
    return instance;
}

ui_thread::ui_thread() {
    auto ready = std::make_shared<std::promise<void>>();
    std::future<void> ready_future = ready->get_future();
    thread_ = std::thread([this, ready] { run([ready] { ready->set_value(); }); });
    ready_future.wait();
}

ui_thread::~ui_thread() {
    shutdown(); // fallback; normally already done by the exit hook
}

void ui_thread::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (app_)
            QMetaObject::invokeMethod(app_, [] { QCoreApplication::exit(0); }, Qt::QueuedConnection);
    }
    if (thread_.joinable() && !on_ui_thread()) thread_.join();
}

void ui_thread::shutdown_at_exit() {
    instance().shutdown();
}

void ui_thread::viewer_created() {
    std::lock_guard<std::mutex> lock(live_mutex_);
    ++live_viewers_;
}

void ui_thread::viewer_destroyed() {
    std::lock_guard<std::mutex> lock(live_mutex_);
    if (--live_viewers_ == 0) {
        // Exit hooks run in reverse registration order. Registering now, after Qt has created
        // whatever global objects it needed for our windows, makes our shutdown run before
        // Qt destroys them. (Registering again later is harmless: shutdown() is idempotent.)
        std::atexit(&ui_thread::shutdown_at_exit);
    }
}

void ui_thread::run(std::function<void()> on_ready) {
    // QApplication must be the first QObject created in the process. Whichever thread
    // creates it becomes Qt's "main thread"; for us, that's this hidden thread.
    static int argc = 1;
    static char app_name[] = "frame_viewer";
    static char* argv[] = {app_name, nullptr};

    {
        QApplication app(argc, argv);
        QApplication::setQuitOnLastWindowClosed(false); // windows come and go; we quit at exit

        {
            std::lock_guard<std::mutex> lock(mutex_);
            app_ = &app;
        }
        on_ready();

        QApplication::exec();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            app_ = nullptr; // no more posts accepted
        }

        // Delete any windows still alive, on this thread, before QApplication goes away.
        std::vector<QWidget*> leftovers(widgets_.begin(), widgets_.end());
        for (QWidget* w : leftovers) delete w;
        widgets_.clear();

        // Run deferred deletes and drop queued functors while the app still exists.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void ui_thread::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!app_) return; // shutting down
    QMetaObject::invokeMethod(app_, std::move(fn), Qt::QueuedConnection);
}

void ui_thread::post_and_wait(std::function<void()> fn) {
    if (on_ui_thread()) {
        fn();
        return;
    }
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> done_future = done->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!app_) return; // shutting down: nothing will run it
        QMetaObject::invokeMethod(
            app_,
            [fn = std::move(fn), done] {
                fn();
                done->set_value();
            },
            Qt::QueuedConnection);
    }
    done_future.wait();
}

} // namespace fv::detail
