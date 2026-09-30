#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>

#include "core/common/log.hpp"
#include "core/common/worker_thread.hpp"

namespace clinicavt::models {

// Keeps a model loaded while wanted. Want loads in the background. Use loads inline if needed and
// retries a failed background load. Release or the idle timeout unloads once no work runs. Want and
// Release never block
class Residency {
   public:
    using Clock = std::chrono::steady_clock;

    Residency(std::function<void()> load, std::function<void()> unload, Clock::duration idle)
        : load_(std::move(load)), unload_(std::move(unload)), idle_(idle) {
        worker_.Start([this] { Run(); });
    }

    ~Residency() {
        worker_.Stop();
    }

    Residency(const Residency&) = delete;
    Residency& operator=(const Residency&) = delete;

    // Also cancels a pending release
    void Want() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_ = Clock::now();
            release_ = false;
            if (!loaded_ && !loading_) want_ = true;
        }
        changed_.notify_all();
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            want_ = false;
            release_ = loaded_ || loading_;
        }
        changed_.notify_all();
    }

    template <typename Work>
    auto Use(Work&& work) -> decltype(work()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return !loading_ && !unloading_; });
            release_ = false;
            if (!loaded_) {
                want_ = false;
                loading_ = true;
                lock.unlock();
                try {
                    load_();
                } catch (...) {
                    lock.lock();
                    loading_ = false;
                    changed_.notify_all();
                    throw;
                }
                lock.lock();
                loading_ = false;
                loaded_ = true;
            }
            ++users_;
        }
        struct Done {
            Residency& self;
            ~Done() {
                {
                    std::lock_guard<std::mutex> lock(self.mutex_);
                    --self.users_;
                    self.last_ = Clock::now();
                }
                self.changed_.notify_all();
            }
        } done{*this};
        return work();
    }

    bool Loaded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return loaded_;
    }

   private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!worker_.Stopping()) {
            if (want_ && !loaded_ && !loading_) {
                want_ = false;
                loading_ = true;
                lock.unlock();
                bool ok = true;
                try {
                    load_();
                } catch (const std::exception& e) {
                    log::Printf("clinicavt-engine: background load failed (%s)\n", e.what());
                    ok = false;
                } catch (...) {
                    ok = false;
                }
                lock.lock();
                loading_ = false;
                loaded_ = ok;
                last_ = Clock::now();
                changed_.notify_all();
                continue;
            }
            const bool idle = Clock::now() - last_ >= idle_;
            if (loaded_ && users_ == 0 && (release_ || idle)) {
                release_ = false;
                loaded_ = false;
                unloading_ = true;
                lock.unlock();
                try {
                    unload_();
                } catch (...) {  // NOLINT(bugprone-empty-catch) freeing is best effort
                }
                lock.lock();
                unloading_ = false;
                changed_.notify_all();
                continue;
            }
            if (loaded_ && users_ == 0) {
                changed_.wait_until(lock, last_ + idle_);
            } else {
                changed_.wait(lock);
            }
        }
    }

    std::function<void()> load_;
    std::function<void()> unload_;
    const Clock::duration idle_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool want_ = false;
    bool release_ = false;
    bool loaded_ = false;
    bool loading_ = false;
    bool unloading_ = false;
    int users_ = 0;
    Clock::time_point last_ = Clock::now();
    WorkerThread worker_{mutex_, changed_};
};

}  // namespace clinicavt::models
