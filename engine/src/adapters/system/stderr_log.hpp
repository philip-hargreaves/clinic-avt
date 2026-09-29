#pragma once

#include <string_view>

#include "ports/log_sink.hpp"

namespace clinicavt::system {

// Engine and host logs go to stderr, which the shell captures to a file
class StderrLog final : public log::ILogSink {
   public:
    void Write(std::string_view line) override;
};

// Installs a StderrLog that lives until the process exits
void LogToStderr();

}  // namespace clinicavt::system
