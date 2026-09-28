#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::note {

// How the note is written: structure and length, each a prompt file so a
// change needs no rebuild. Values are validated at the RPC boundary.
struct NoteOptions {
    std::string style = "prose";     // prose | soap
    std::string detail = "concise";  // concise | detailed
    bool confirmed = false;          // the clinician says it is a consultation: no refusal
};

// Streams partials, returns the note, throws on failure. Cancel
// interrupts from another thread
class INoteWriter {
   public:
    using Progress = std::function<void(const std::string&)>;

    virtual ~INoteWriter() = default;

    virtual std::string Write(const std::vector<asr::Turn>& transcript, const NoteOptions& options,
                              const Progress& progress) = 0;

    // Patient information from the finished note, on the same model. A
    // writer that only writes notes returns false and is still valid
    virtual bool WritesPatient() const {
        return false;
    }

    virtual std::string WritePatient(const std::string&, const Progress&) {
        return {};
    }

    // Empty means no title and the shell shows the date, so failure is never an error
    virtual std::string WriteLabel(const std::string&) {
        return {};
    }

    // Anonymised case summary from the note. Throws on failure
    virtual std::string WriteSummary(const std::string&) {
        return {};
    }

    // Starts the background model load. Idempotent
    virtual void Prepare() {}

    // Capture-phase guess at how the sealed transcript begins, so the stop
    // path prefills only the tail. Never blocks capture
    virtual void Prefill(const std::vector<asr::Turn>&, const NoteOptions&) {}

    virtual void Cancel() {}
};

}  // namespace clinicavt::note
