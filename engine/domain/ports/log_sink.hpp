#pragma once

#include <string_view>

namespace clinicavt::log {

// Destination for diagnostic lines. Each executable installs one at startup
class ILogSink {
   public:
    virtual ~ILogSink() = default;
    // One whole line, newline included. Called from any thread
    virtual void Write(std::string_view line) = 0;
};

}  // namespace clinicavt::log
