#pragma once

#include <functional>
#include <string>

namespace clinicavt::note {

// Configured tier and whether its model is loaded. The shell status is built from it
struct NoteModelState {
    enum class Phase { kIdle, kLoading, kReady, kFailed };

    Phase phase = Phase::kIdle;
    std::string tier = "default";
    std::string id;          // the model the tier resolves to, empty when none is staged
    std::string name;        // its display name
    std::string detail;      // the reason, on kFailed
    double seconds = 0;      // verify + load, on kReady
    bool first_use = false;  // no compile cache yet, so the load takes minutes
};

// Note tier configuration. A tier is a role the model store resolves. It is separate from
// INoteWriter so single-model writers do not need it
class INoteTiers {
   public:
    using Listener = std::function<void(const NoteModelState&)>;

    virtual ~INoteTiers() = default;

    // Idempotent. A different tier unloads the current model and loads the new one.
    // Throws std::invalid_argument if no model claims the tier (listing staged ones),
    // std::logic_error while a load runs (loads cannot be cancelled)
    virtual NoteModelState Configure(const std::string& tier) = 0;

    virtual NoteModelState State() const = 0;
};

}  // namespace clinicavt::note
