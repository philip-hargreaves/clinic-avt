#pragma once

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/audio/level_meter.hpp"
#include "core/audio/voice_enrolment.hpp"
#include "ports/audio_source.hpp"
#include "ports/session_store.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::session {

// Session events, delivered on the audio pipeline thread behind the capture ring
class ISessionEvents {
   public:
    virtual ~ISessionEvents() = default;

    virtual void OnLevel(const audio::LevelReading& reading) = 0;

    virtual void OnInterrupted(audio::SourceEndReason reason, const std::string& detail) = 0;

    // Finalise stages as they start ("transcript", "speakers"), on the
    // stopping thread, and only for work that actually runs
    virtual void OnProgress(const std::string&) {}

    // Enrolment: the level and speech captured so far, then the outcome
    virtual void OnEnrolProgress(const audio::EnrolProgress&) {}
    virtual void OnEnrolDone(bool, const std::string&, double) {}

    // The note lane, delivered on its own thread after finalise
    virtual void OnNotePartial(const std::string&) {}
    virtual void OnNoteReady(const std::string&) {}
    virtual void OnNoteFailed(const std::string&) {}
    // The note as stored, with its revision, for work that follows the note
    virtual void OnNoteSaved(const std::string& /*session*/, const store::Document& /*note*/) {}

    // The store could not write (disk full, I/O). Recording continues
    virtual void OnStorageFault(const std::string& /*detail*/) {}
    // No note, and why: too thin to write from (not overridable) or the model
    // says it was not a consultation (the clinician can insist)
    virtual void OnNoteRefused(const std::string&, bool) {}

    // Patient information follows the note on the same thread
    virtual void OnPatientPartial(const std::string&) {}
    virtual void OnPatientReady(const std::string&) {}
    virtual void OnPatientFailed(const std::string&) {}

    // The appraisal case summary, written on request for a stored session
    virtual void OnSummaryReady(const std::string& /*session*/, const std::string& /*text*/) {}
    virtual void OnSummaryFailed(const std::string& /*session*/, const std::string& /*detail*/) {}
};

// A replay request, carried into the source factory. Absent means microphone
struct ReplaySpec {
    std::string path;
    double speed = 1.0;
    std::uint64_t start_frame = 0;  // resume: skip audio already captured
};

// Resolved by the caller: the id pins the endpoint (empty = default), the
// name goes into the session snapshot
struct MicSelection {
    std::string id;
    std::string name;
};

using SourceFactory = std::function<std::unique_ptr<audio::IAudioSource>(
    const std::optional<ReplaySpec>&, const std::string& mic_id)>;

// Every store failure is logged. A full or failing disk is announced
inline void ReportStoreFailure(ISessionEvents& events, const char* what, const std::exception& e) {
    std::fprintf(stderr, "clinicavt-engine: store %s failed: %s\n", what, e.what());
    const auto* fault = dynamic_cast<const store::StoreError*>(&e);
    if (fault != nullptr &&
        (fault->Code() == store::StoreCode::kFull || fault->Code() == store::StoreCode::kIo)) {
        events.OnStorageFault(e.what());
    }
}

}  // namespace clinicavt::session
