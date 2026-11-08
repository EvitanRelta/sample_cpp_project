#include "fv/viewer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace fv {

BufferPool::BufferPool(cv::Size size, int type, int initial_count, int max_count)
    : m_size(size), m_type(type), m_max_count(std::max(max_count, std::max(initial_count, 1))) {
    for (int i = 0; i < std::max(initial_count, 1); ++i) m_buffers.emplace_back(m_size, m_type);
}

bool BufferPool::is_free(const cv::Mat& buffer) {
    // cv::Mat counts every header sharing its pixels. The pool's own header is one, so a count
    // of 1 means no one else (viewer, paint in progress, or caller) can be reading it.
    // Acquire ordering: the viewer's last reads happen-before its release of the reference,
    // which matters on weakly ordered CPUs (e.g. ARM).
    return buffer.u != nullptr && __atomic_load_n(&buffer.u->refcount, __ATOMIC_ACQUIRE) == 1;
}

cv::Mat BufferPool::acquire() {
    bool warned = false;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::size_t n = m_buffers.size();
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t i = (m_next + k) % n;
                if (is_free(m_buffers[i])) {
                    m_next = (i + 1) % n;
                    return m_buffers[i]; // shallow copy: the caller's header marks it busy
                }
            }
            if (static_cast<int>(n) < m_max_count) {
                m_buffers.emplace_back(m_size, m_type);
                return m_buffers.back();
            }
        }
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "fv::BufferPool: all %d buffers are in use; waiting. Are you holding on to "
                         "buffers from acquire()?\n",
                         m_max_count);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int BufferPool::buffer_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_buffers.size());
}

} // namespace fv
