#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ports/store_error.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::store {

using SessionId = std::string;

struct SessionMeta {
    int sample_rate = 0;
    std::string device_id;
    std::string device_name;
    bool retain = true;      // false: erased once the consultation is left
    std::string started_at;  // ISO 8601 UTC, empty: now
};

enum class SessionState { kRecording, kFinalised };

struct SessionSummary {
    SessionId id;
    std::string started_at;  // ISO 8601 UTC
    std::string ended_at;    // Empty while recording or after a crash
    SessionState state = SessionState::kRecording;
    int sample_rate = 0;
    std::string label;            // The consultation's name, empty until a note exists
    std::string edited_at;        // Latest clinician edit to the note or patient information
    double audio_seconds = 0;     // From the turn timings
    bool has_reflection = false;  // Has an appraisal document
    bool sample = false;          // Seeded, never a real consultation
    bool cleared = false;         // As ISessionCatalog::Cleared
    std::string written_at;       // Latest write of any document, empty when none
};

// Stored finalised and flagged as a sample
struct SessionSeed {
    std::string started_at;  // ISO 8601 UTC
    std::string ended_at;
    int sample_rate = 16000;
    std::vector<asr::Turn> turns;
};

// At most one of each kind per session. Summary is the anonymised case study and label the
// consultation's name. Guidance holds the passages the note search showed
enum class DocumentKind { kNote, kPatient, kTranslation, kLabel, kSummary, kReflection, kGuidance };

inline constexpr std::array kDocumentKinds{DocumentKind::kNote,        DocumentKind::kPatient,
                                           DocumentKind::kTranslation, DocumentKind::kLabel,
                                           DocumentKind::kSummary,     DocumentKind::kReflection,
                                           DocumentKind::kGuidance};

// The appraisal entry, which is not part of the clinical record
inline constexpr std::array kAppraisalKinds{DocumentKind::kSummary, DocumentKind::kReflection};

inline constexpr std::array kKeptOnClear{DocumentKind::kLabel, DocumentKind::kSummary,
                                         DocumentKind::kReflection};

inline constexpr bool IsAppraisal(DocumentKind kind) {
    return std::ranges::find(kAppraisalKinds, kind) != kAppraisalKinds.end();
}

inline constexpr bool KeptOnClear(DocumentKind kind) {
    return std::ranges::find(kKeptOnClear, kind) != kKeptOnClear.end();
}

struct Document {
    std::string text;             // Empty when the session has no such document
    std::string language = "en";  // A language name for a translation
    std::string style;            // Note only: prose or soap
    std::string detail;           // Note only: concise or detailed
    std::string generated_at;     // For a reflection, when it was created
    std::string edited_at;        // Empty until the clinician edits it
    // The sequence the text is sealed at, 0 when absent. The first write picks a random value and
    // each rewrite adds one. A count from 1 would reuse an IV after a delete and rewrite
    std::int64_t revision = 0;
};

struct RecordDocument {
    DocumentKind kind = DocumentKind::kNote;
    Document document;  // revision as stored, so guidance staleness survives a backup
};

// A finalised session without audio, as one backup entry holds it
struct SessionRecord {
    SessionId id;
    std::string started_at;
    std::string ended_at;
    int sample_rate = 0;
    std::string device_id;
    std::string device_name;
    std::uint64_t lost_frames = 0;
    std::vector<asr::Turn> turns;
    std::vector<RecordDocument> documents;  // one per kind present
};

enum class AddOutcome {
    kAdded,
    kCompleted,  // a cleared session restored with what it had lost
    kSkipped,    // already stored, nothing written
};

// Audio is committed every second and kept only to resume after a crash. Finalise stores the
// transcript and erases the audio, Cancel keeps nothing, and a session left recording is listed
// for recovery
class IRecordingStore {
   public:
    virtual ~IRecordingStore() = default;

    virtual SessionId Begin(const SessionMeta& meta) = 0;

    // lost_frames: frames missing just before these
    virtual void Append(const SessionId& id, std::span<const float> frames,
                        std::uint64_t lost_frames) = 0;

    // Written at finalise in one transaction, before the session seals
    virtual void ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) = 0;

    virtual void Finalise(const SessionId& id) = 0;
    virtual void Cancel(const SessionId& id) = 0;

    virtual void Abandon(const SessionId& id) = 0;

    // Stored capture in order, for resuming a crashed session. Refuses the session that is
    // recording
    virtual std::vector<float> ReadAudio(const SessionId& id) = 0;

    // Called off the caller thread when an audio commit fails. Recording continues and retries
    // NOLINTNEXTLINE(performance-unnecessary-value-param) the store keeps the sink
    virtual void SetFaultListener(std::function<void(const StoreError&)>) {}
};

class IDocumentStore {
   public:
    virtual ~IDocumentStore() = default;

    // Save stores a generation, Edit the clinician's text over it. Reading an
    // unknown session throws
    virtual void SaveDocument(const SessionId& id, DocumentKind kind, const Document& document) = 0;
    virtual void EditDocument(const SessionId& id, DocumentKind kind, const std::string& text) = 0;
    virtual Document ReadDocument(const SessionId& id, DocumentKind kind) = 0;

    // Removes one kind. A kind the session never had is not an error
    virtual void DeleteDocument(const SessionId& id, DocumentKind kind) = 0;
};

class ISessionCatalog {
   public:
    virtual ~ISessionCatalog() = default;

    virtual std::vector<SessionSummary> ListSessions() = 0;

    // These refuse the session that is recording
    virtual std::vector<asr::Turn> ReadTurns(const SessionId& id) = 0;
    virtual void Delete(const SessionId& id) = 0;

    // Erases finalised retain-off sessions. Crashed ones are kept for recovery, so
    // the setting never loses audio
    virtual void EraseUnretained() = 0;

    // Crypto-erases all sessions except one still recording. With keep_reflections,
    // sessions with an appraisal entry are cleared instead. Returns the count
    virtual std::size_t DeleteAll(bool keep_reflections = false) = 0;

    // Erases all of a finalised session except the kKeptOnClear documents, which are resealed
    // under a new key. A session without an appraisal document is deleted
    virtual void Clear(const SessionId& id) = 0;

    // Finalised, with no turns and no document outside kKeptOnClear
    virtual bool Cleared(const SessionId& id) = 0;
};

class ISampleStore {
   public:
    virtual ~ISampleStore() = default;

    // ClearDemo removes every seeded sample
    virtual SessionId Seed(const SessionSeed& seed) = 0;
    virtual std::size_t ClearDemo() = 0;
};

// Whole sessions without audio, for backup and restore
class IRecordStore {
   public:
    virtual ~IRecordStore() = default;

    // For backup. Throws for an unknown or recording session
    virtual SessionRecord ReadRecord(const SessionId& id) = 0;

    // Adds a session from a backup in one transaction under a new key, finalised and saved, with
    // its times and revisions. A cleared session with the same id gets back its transcript and
    // missing documents and keeps its own. Any other stored id is skipped. Throws for a record
    // ValidRecord rejects
    virtual AddOutcome AddRecord(const SessionRecord& record) = 0;
};

class ISessionStore : public IRecordingStore,
                      public IDocumentStore,
                      public ISessionCatalog,
                      public ISampleStore,
                      public IRecordStore {};

}  // namespace clinicavt::store
