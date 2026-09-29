#pragma once

#include <functional>

#include "adapters/ipc/handlers.hpp"

namespace clinicavt::ipc {

inline constexpr const char* kArchiveRunning = "a backup or restore is running";

// True while a backup or restore runs, when deletes are refused
std::function<bool()> ArchiveBusy(const EngineServices& services);

// Without a note writer (CI), send empty note/ready and patient/ready so the
// shell contract holds
void StubDocuments(const Notify& notify);

}  // namespace clinicavt::ipc
