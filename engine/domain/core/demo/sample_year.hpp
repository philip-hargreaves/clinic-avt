#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

#include "ports/reflection_codec.hpp"
#include "ports/sample_source.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::demo {

// Start time: months_back months before now, the day clamped to the month
std::chrono::sys_seconds SampleStart(const Sample& sample, std::chrono::sys_seconds now);

std::size_t SeedSampleYear(store::ISessionStore& sessions, const records::IReflectionCodec& codec,
                           const std::vector<Sample>& samples, std::chrono::sys_seconds now);

bool HasSamples(store::ISessionStore& sessions);

// Seeds and clears the sample year alongside real sessions
class DemoSamples {
   public:
    DemoSamples(store::ISessionStore& sessions, const records::IReflectionCodec& codec,
                ISampleSource& source)
        : sessions_(sessions), codec_(codec), source_(source) {}

    // A no-op returning 0 when samples are stored already. Throws when the source has none
    std::size_t SeedOnce(std::chrono::sys_seconds now);

    // Removes only the samples and returns how many
    std::size_t Clear();

   private:
    store::ISessionStore& sessions_;
    const records::IReflectionCodec& codec_;
    ISampleSource& source_;
};

}  // namespace clinicavt::demo
