#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::note {

enum class NoteStyle { kProse, kSoap };
enum class NoteDetail { kConcise, kDetailed };

// Names used by the prompt files, the store and the wire
inline const char* NoteStyleName(NoteStyle style) {
    return style == NoteStyle::kSoap ? "soap" : "prose";
}

inline const char* NoteDetailName(NoteDetail detail) {
    return detail == NoteDetail::kDetailed ? "detailed" : "concise";
}

inline std::optional<NoteStyle> NoteStyleFrom(std::string_view name) {
    if (name == "prose") return NoteStyle::kProse;
    if (name == "soap") return NoteStyle::kSoap;
    return std::nullopt;
}

inline std::optional<NoteDetail> NoteDetailFrom(std::string_view name) {
    if (name == "concise") return NoteDetail::kConcise;
    if (name == "detailed") return NoteDetail::kDetailed;
    return std::nullopt;
}

// Note structure and length; each maps to a prompt file so changes need no
// rebuild. Validated at the RPC boundary
struct NoteOptions {
    NoteStyle style = NoteStyle::kProse;
    NoteDetail detail = NoteDetail::kConcise;
    bool confirmed = false;  // the clinician confirmed a consultation, so the note is never refused
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
