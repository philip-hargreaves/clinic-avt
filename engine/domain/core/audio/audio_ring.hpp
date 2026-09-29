#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <span>
#include <vector>

namespace clinicavt::audio {

// SPSC Lamport ring with monotonic indices masked on access. Capacity is
// rounded to a power of two
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas padding is intended
class AudioRing {
   public:
    explicit AudioRing(std::size_t min_capacity)
        : capacity_(std::bit_ceil(min_capacity)), mask_(capacity_ - 1), buffer_(capacity_) {}

    std::size_t Capacity() const {
        return capacity_;
    }

    // Called by the producer only. Returns frames written. Fewer than frames.size() means the ring
    // is full
    std::size_t TryPush(std::span<const float> frames) {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t read = read_.load(std::memory_order_acquire);
        const std::size_t free = capacity_ - (write - read);
        const std::size_t count = std::min(frames.size(), free);
        // At most two segments: up to the end of the buffer, then from its start
        const std::size_t at = write & mask_;
        const std::size_t first = std::min(count, capacity_ - at);
        std::copy_n(frames.begin(), first, buffer_.begin() + static_cast<std::ptrdiff_t>(at));
        std::copy_n(frames.begin() + static_cast<std::ptrdiff_t>(first), count - first,
                    buffer_.begin());
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    // Called by the consumer only. Returns frames read
    std::size_t TryPop(std::span<float> out) {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        const std::size_t write = write_.load(std::memory_order_acquire);
        const std::size_t available = write - read;
        const std::size_t count = std::min(out.size(), available);
        const std::size_t at = read & mask_;
        const std::size_t first = std::min(count, capacity_ - at);
        std::copy_n(buffer_.begin() + static_cast<std::ptrdiff_t>(at), first, out.begin());
        std::copy_n(buffer_.begin(), count - first,
                    out.begin() + static_cast<std::ptrdiff_t>(first));
        read_.store(read + count, std::memory_order_release);
        return count;
    }

   private:
    std::size_t capacity_;
    std::size_t mask_;
    std::vector<float> buffer_;
    // On separate cache lines so the two sides never false-share
    alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> write_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> read_{0};
};
#pragma warning(pop)

}  // namespace clinicavt::audio
