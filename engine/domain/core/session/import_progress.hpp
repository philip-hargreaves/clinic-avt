#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ports/session_store.hpp"

namespace clinicavt::session {

// Import stages and their share of the overall percentage: reading 0-5 %, speech 5-15 %,
// transcribing 15-95 %, finalising 95-100 %
enum class ImportStage { kReading, kSpeech, kTranscribing, kFinalising };

int ImportPercent(ImportStage stage, double fraction);

// Called on the import thread. done gets an empty error on success, else why the session was
// erased
struct ImportReport {
    std::function<void(const store::SessionId&, ImportStage stage, int percent)> progress;
    std::function<void(const store::SessionId&, const std::string& error)> done;
};

// Reads a recording, reporting the fraction read
using ImportRead = std::function<std::vector<float>(const std::function<void(double)>& progress)>;

// done() error for an import stopped by Cancel
inline constexpr const char* kImportCancelled = "cancelled";

}  // namespace clinicavt::session
