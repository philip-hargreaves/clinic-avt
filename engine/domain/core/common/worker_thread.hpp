#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace clinicavt {

// Shares the owner's mutex and condition variable, so a loop can wait on Stopping() and its own
// state together
class WorkerThread {
   public:
    WorkerThread(std::mutex& mutex, std::condition_variable& wake) : mutex_(mutex), wake_(wake) {}

    ~WorkerThread() {
        Stop();
    }

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    // The previous thread must have been joined
    void Start(std::function<void()> loop) {
        thread_ = std::thread(std::move(loop));
    }

    bool Started() const {
        return thread_.joinable();
    }

    // Caller holds the mutex
    bool Stopping() const {
        return stop_;
    }

    // Caller holds the mutex. Lets a stopped worker start again
    void ClearStop() {
        stop_ = false;
    }

    // Two threads may call this at once. Only the first joins the thread, and the second can
    // return before it ends
    void Stop() {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            thread = std::move(thread_);
        }
        wake_.notify_all();
        if (thread.joinable()) thread.join();
    }

   private:
    std::mutex& mutex_;
    std::condition_variable& wake_;
    bool stop_ = false;  // under mutex_
    std::thread thread_;
};

}  // namespace clinicavt
