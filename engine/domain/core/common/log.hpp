#pragma once

#include "ports/log_sink.hpp"

#if defined(__clang__) || defined(__GNUC__)
#define CLINICAVT_PRINTF_FORMAT(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define CLINICAVT_PRINTF_FORMAT(format_index, first_arg)
#endif

namespace clinicavt::log {

// The sink must outlive every thread that logs. Null, the default, drops lines
void SetSink(ILogSink* sink);

// printf-style. The format includes the process prefix and the trailing newline, and sinks
// write the line as given
void Printf(const char* format, ...) CLINICAVT_PRINTF_FORMAT(1, 2);

}  // namespace clinicavt::log
