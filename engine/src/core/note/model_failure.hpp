#pragma once

#include <string>
#include <string_view>

#include "core/common/strings.hpp"

namespace clinicavt::note {

// Why a note model would not load, read from OpenVINO's message
enum class LoadFailure {
    kMemory,  // an allocation refused: retrying cannot help until memory frees
    kCache,   // a damaged compile cache: it never repairs itself
    kOther,
};

inline LoadFailure ClassifyLoadFailure(std::string_view detail) {
    if (strings::Contains(detail, "Can not allocate") ||
        strings::Contains(detail, "bad allocation") ||
        strings::Contains(detail, "CL_OUT_OF_HOST_MEMORY") ||
        strings::Contains(detail, "error: -6")) {
        return LoadFailure::kMemory;
    }
    if (strings::Contains(detail, "ProgramBuilder build failed") ||
        strings::Contains(detail, "Failed to create program")) {
        return LoadFailure::kCache;
    }
    return LoadFailure::kOther;
}

// After these the driver context is corrupt, and any further GPU call from
// the same process can hang it
inline bool PoisonsGpuContext(std::string_view detail) {
    return strings::Contains(detail, "CL_OUT_OF_RESOURCES") ||
           strings::Contains(detail, "error: -5") || strings::Contains(detail, "code: -5") ||
           strings::Contains(detail, "could not execute a primitive") ||
           strings::Contains(detail, "DEVICE_LOST") || strings::Contains(detail, "device lost");
}

// A note host stuck in a driver call holds the GPU until the computer restarts
inline constexpr const char* kStuckInDriver =
    "the note process is stuck in the graphics driver. Use Restart on the power menu to free it";

inline std::string PlainLoadMessage(LoadFailure failure, const std::string& model) {
    const std::string name = model.empty() ? "the note model" : model;
    switch (failure) {
        case LoadFailure::kMemory:
            return "There is not enough free memory to load " + name +
                   ". Close other programs or free some disk space, or choose a smaller model";
        case LoadFailure::kCache:
            return name + " could not be prepared on this computer. Restart the app to try again";
        default:
            return name + " could not be loaded";
    }
}

}  // namespace clinicavt::note
