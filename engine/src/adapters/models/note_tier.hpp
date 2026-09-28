#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "adapters/models/model_store.hpp"

namespace clinicavt::models {

// The shell's value for a note model the user never chose
inline constexpr const char* kAutoNoteTier = "auto";

// The integrated GPU shares RAM, so the machine's memory decides: the 9B's host
// peaks near 7.6 GB beside the engine's 4.5-7.5 GB. The GPU's own limit is checked too
inline constexpr std::uint64_t kNineBInstalledBytes = 23ULL << 30;  // a 24 GB machine
inline constexpr std::uint64_t kNineBGpuBytes = 11ULL << 30;

struct MachineMemory {
    std::optional<std::uint64_t> installed;
    std::optional<std::uint64_t> gpu;
};

inline std::vector<std::string> StagedNoteTiers(const ModelStore& store) {
    std::vector<std::string> tiers;
    for (const auto& model : store.List()) {
        if (model.task == "note") tiers.push_back(model.tier);
    }
    return tiers;
}

// The note model a machine starts on until the user chooses one: the 9B when the machine
// can hold it, else the 4B, else whatever is staged. The 35B only when nothing else is.
// Empty when no note model is staged. Unknown memory counts as too little
inline std::string AutoNoteTier(const std::vector<std::string>& staged,
                                const MachineMemory& memory) {
    const auto has = [&staged](const char* tier) {
        return std::find(staged.begin(), staged.end(), tier) != staged.end();
    };
    const bool fits_nine_b = memory.installed.value_or(0) >= kNineBInstalledBytes &&
                             memory.gpu.value_or(0) >= kNineBGpuBytes;
    if (has("default") && fits_nine_b) return "default";
    for (const char* tier : {"constrained", "default", "accuracy"}) {
        if (has(tier)) return tier;
    }
    return {};
}

}  // namespace clinicavt::models
