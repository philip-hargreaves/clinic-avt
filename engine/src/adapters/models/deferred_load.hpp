#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/common/log.hpp"
#include "core/metrics/metrics.hpp"

namespace clinicavt::models {

// Builds T on a background thread. Get waits and rethrows a load failure.
// Success records the load seconds under `name`
template <typename T>
class DeferredLoad {
   public:
    DeferredLoad(std::string name, std::function<std::unique_ptr<T>()> build,
                 metrics::Registry* metrics = nullptr)
        : name_(std::move(name)) {
        loader_ = std::thread([this, build = std::move(build), metrics] {
            const auto t0 = std::chrono::steady_clock::now();
            try {
                built_ = build();
                const double seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                log::Printf("clinicavt-engine: %s ready in %.1f s\n", name_.c_str(), seconds);
                if (metrics != nullptr) metrics->RecordLoad(name_, seconds);
            } catch (const std::exception& e) {
                log::Printf("clinicavt-engine: %s unavailable (%s)\n", name_.c_str(), e.what());
                error_ = std::current_exception();
            } catch (...) {
                error_ = std::current_exception();
            }
            ready_.store(true);
        });
    }

    ~DeferredLoad() {
        if (loader_.joinable()) {
            loader_.join();
        }
    }

    T& Get() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (loader_.joinable()) {
                loader_.join();
            }
        }
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
        return *built_;
    }

    // Non-blocking; true only if the build succeeded
    bool Loaded() const {
        return ready_.load() && error_ == nullptr;
    }

    // Non-blocking; true once the build ends, either way
    bool Settled() const {
        return ready_.load();
    }

   private:
    std::string name_;
    std::unique_ptr<T> built_;
    std::exception_ptr error_;
    std::atomic<bool> ready_{false};
    std::mutex mutex_;
    std::thread loader_;
};

}  // namespace clinicavt::models
