#pragma once

#include <string>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::demo {

// A seeded consultation from demo/reflections. It holds the app's transcript of a real recording,
// notes from its best model and hand-written answers. Dates are relative to the seed time
struct Sample {
    std::string source;
    int months_back = 0;
    int day = 1;
    int hour = 9;
    int minute = 0;
    std::string label;
    std::string note;
    std::string patient;
    std::string summary;
    double audio_seconds = 0;
    std::vector<asr::Turn> turns;
    std::string happened;
    std::string learned;
    std::string next;
};

class ISampleSource {
   public:
    virtual ~ISampleSource() = default;
    // Empty when none are installed. Throws when one cannot be read
    virtual std::vector<Sample> Load() = 0;
};

}  // namespace clinicavt::demo
