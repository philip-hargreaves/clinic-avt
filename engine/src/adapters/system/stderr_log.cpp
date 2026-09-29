#include "adapters/system/stderr_log.hpp"

#include <cstdio>

#include "core/common/log.hpp"

namespace clinicavt::system {

void StderrLog::Write(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stderr);
}

void LogToStderr() {
    // Never destroyed, so a thread that logs during exit still has a sink
    static auto* const kSink = new StderrLog;
    log::SetSink(kSink);
}

}  // namespace clinicavt::system
