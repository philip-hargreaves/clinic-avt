#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::note {

// Note structure and length; each maps to a prompt file so changes need no
// rebuild. Validated at the RPC boundary
struct NoteOptions {
    std::string style = "prose";     // prose | soap
    std::string detail = "concise";  // concise | detailed
    bool confirmed = false;          // clinician confirmed it is a consultation, so no refusal
};

// Streams partials, returns the note, throws on failure. Cancel
// interrupts from another thread
class INoteWriter {
   public:
    using Progress = std::function<void(const std::string&)>;

    virtual ~INoteWriter() = default;

    virtual std::string Write(const std::vector<asr::Turn>& transcript, const NoteOptions& options,
                              const Progress& progress) = 0;

    // Patient information from the finished note, same model. Writers that only
    // write notes return false
    virtual bool WritesPatient() const {
        return false;
    }

    virtual std::string WritePatient(const std::string&, const Progress&) {
        return {};
    }

    // Empty means no title (shell shows the date), so failure is not an error
    virtual std::string WriteLabel(const std::string&) {
        return {};
    }

    // Anonymised case summary from the note. Throws on failure
    virtual std::string WriteSummary(const std::string& note) = 0;

    // Starts the background model load. Idempotent
    virtual void Prepare() {}

    // Capture-phase guess at the start of the sealed transcript, so stop only
    // prefills the tail. Must not block capture
    virtual void Prefill(const std::vector<asr::Turn>&, const NoteOptions&) {}

    virtual void Cancel() {}
};

}  // namespace clinicavt::note
