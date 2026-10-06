#pragma once

#include <cstddef>
#include <deque>

namespace clinicavt::metrics {

// Tokens per second at the source, before throttling. Rate() is a rolling
// window for display, Average() the whole stream
class ThroughputMeter {
   public:
    static constexpr double kWindowSeconds = 2.0;

    void Token(double now) {
        if (total_ == 0) {
            first_ = now;
        }
        last_ = now;
        ++total_;
        stamps_.push_back(now);
        Trim(now);
    }

    // (count-1)/span over the window, since one token is not a rate. A stalled
    // stream decays to zero as the window empties
    double Rate(double now) {
        Trim(now);
        if (stamps_.size() < 2) {
            return 0;
        }
        const double span = now - stamps_.front();
        return span <= 0 ? 0 : static_cast<double>(stamps_.size() - 1) / span;
    }

    double Average() const {
        if (total_ < 2 || last_ <= first_) {
            return 0;
        }
        return static_cast<double>(total_ - 1) / (last_ - first_);
    }

   private:
    void Trim(double now) {
        while (!stamps_.empty() && now - stamps_.front() > kWindowSeconds) {
            stamps_.pop_front();
        }
    }

    std::deque<double> stamps_;
    std::size_t total_ = 0;
    double first_ = 0;
    double last_ = 0;
};

}  // namespace clinicavt::metrics
