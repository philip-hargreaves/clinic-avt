#pragma once

#include <cstdint>
#include <optional>

namespace clinicavt::system {

std::optional<std::uint64_t> InstalledMemoryBytes();

// Intel GPU dedicated + shared memory as Windows reports it, so driver overrides
// of the shared limit are included
std::optional<std::uint64_t> IntelGpuMemoryBytes();

}  // namespace clinicavt::system
