#pragma once

#include <cstdint>

#include "ports/audio_source.hpp"

namespace clinicavt::audio {

// Computes lost frames from WASAPI packet positions only. Positions restart when the client is
// rebuilt, so each stream needs its own instance
class CaptureTimeline {
   public:
    explicit CaptureTimeline(std::uint32_t native_rate) : native_rate_(native_rate) {}

    // Frames lost before this packet. The first packet anchors the origin and
    // never reports loss
    std::uint64_t OnPacket(std::uint64_t device_position, std::uint32_t frames) {
        std::uint64_t lost = 0;
        if (!have_baseline_) {
            have_baseline_ = true;
            origin_ = device_position;
        } else {
            // Cumulative gap in native*target units, so both rates stay integral
            const std::int64_t native_delta =
                static_cast<std::int64_t>(device_position) - static_cast<std::int64_t>(origin_);
            const std::int64_t scaled =
                native_delta * kSampleRate -
                static_cast<std::int64_t>(delivered_) * static_cast<std::int64_t>(native_rate_);

            // Round to the nearest target frame so sub-frame wobble from a
            // non-integral rate ratio never reads as loss
            const std::int64_t half = static_cast<std::int64_t>(native_rate_) / 2;
            const std::int64_t cumulative =
                (scaled + (scaled >= 0 ? half : -half)) / static_cast<std::int64_t>(native_rate_);
            if (cumulative > static_cast<std::int64_t>(total_lost_)) {
                lost = static_cast<std::uint64_t>(cumulative) - total_lost_;
                total_lost_ += lost;
            }
        }

        delivered_ += frames;
        return lost;
    }

   private:
    std::uint32_t native_rate_;
    bool have_baseline_ = false;
    std::uint64_t origin_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t total_lost_ = 0;
};

}  // namespace clinicavt::audio
