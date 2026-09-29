#pragma once

#include <string>

#include "ports/audio_source.hpp"
#include "ports/session_store.hpp"

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
    virtual void OnNoteSaved(const std::string& /*session*/, const store::Document& /*note*/) {}

    // The store could not write (disk full, I/O). Recording continues
    virtual void OnStorageFault(const std::string& /*detail*/) {}
    // No note was written. overridable is true when the model judged it not a consultation and
    // false when the transcript was too short
    virtual void OnNoteRefused(const std::string&, bool) {}

    // Patient information follows the note on the same thread
    virtual void OnPatientPartial(const std::string&) {}
    virtual void OnPatientReady(const std::string&) {}
    virtual void OnPatientFailed(const std::string&) {}

    // The appraisal case summary, written on request for a stored session
    virtual void OnSummaryReady(const std::string& /*session*/, const std::string& /*text*/) {}
    virtual void OnSummaryFailed(const std::string& /*session*/, const std::string& /*detail*/) {}
};

}  // namespace clinicavt::session
