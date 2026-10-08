#pragma once

#include <condition_variable>
#include <mutex>

namespace Common {

class Event {
public:
    void Set() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_set = true;
        m_condition.notify_one();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock, [this] { return m_set; });
        m_set = false;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_set = false;
};

}
