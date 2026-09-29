#include "core/common/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>

namespace clinicavt::log {

namespace {

std::atomic<ILogSink*> g_sink{nullptr};

}  // namespace

void SetSink(ILogSink* sink) {
    g_sink.store(sink);
}

void Printf(const char* format, ...) {
    ILogSink* const sink = g_sink.load();
    if (sink == nullptr) return;
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    const int size = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (size > 0) {
        std::string line(static_cast<std::size_t>(size), '\0');
        std::vsnprintf(line.data(), line.size() + 1, format, args);
        sink->Write(line);
    }
    va_end(args);
}

}  // namespace clinicavt::log
