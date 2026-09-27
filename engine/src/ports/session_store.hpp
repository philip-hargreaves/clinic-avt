#pragma once

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
    bool retain = true;  // false: erased once the consultation is left
};

struct SessionSummary {
    SessionId id;
    std::string started_at;  // ISO 8601 UTC
    std::string ended_at;    // Empty while recording or after a crash
    std::string state;       // recording | finalised
    int sample_rate = 0;
    std::string label;            // The consultation in a line, empty until a note exists
    std::string edited_at;        // Latest clinician edit to note or sheet, empty when none
    double audio_seconds = 0;     // The consultation's audio length, from the sealed turns
    bool has_reflection = false;  // An appraisal entry exists: a summary or a reflection
    bool demo = false;            // A seeded sample rather than a real record
    // Cleared: the consultation was removed and only its appraisal entry and label were kept.
    // History does not list it; the appraisal journal does
    bool cleared = false;
    std::string written_at;  // Latest write of any document, ISO 8601 UTC, empty when none
};

// A seeded session: finalised, with given times, flagged for clearing
struct SessionSeed {
    std::string started_at;  // ISO 8601 UTC
    std::string ended_at;
    int sample_rate = 16000;
    std::vector<asr::Turn> turns;
};

// The texts a finalised session holds, one of each. A rewrite replaces. Summary and
// reflection are appraisal documents outside the clinical record. Guidance is what the
// note's search showed
enum class DocumentKind { kNote, kPatient, kTranslation, kLabel, kSummary, kReflection, kGuidance };

// Every kind, so a whole-session read or write never misses one
inline constexpr std::array kDocumentKinds{DocumentKind::kNote,        DocumentKind::kPatient,
                                           DocumentKind::kTranslation, DocumentKind::kLabel,
                                           DocumentKind::kSummary,     DocumentKind::kReflection,
                                           DocumentKind::kGuidance};

struct Document {
    std::string text;             // Empty when the session has no such document
    std::string language = "en";  // BCP 47
    std::string style;            // Note only: prose | soap
    std::string detail;           // Note only: concise | standard | detailed
    std::string generated_at;     // ISO 8601 UTC, when the model wrote it
    std::string edited_at;        // ISO 8601 UTC, empty until a person changed it
    std::int64_t revision = 0;    // Counts every write, 0 when absent
};

// One document as stored, for moving a session whole
struct RecordDocument {
    DocumentKind kind = DocumentKind::kNote;
    Document document;  // revision kept as stored, so guidance staleness survives a move
};

// A finalised session whole, without audio: what a backup holds for one consultation
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

// What adding a record did
enum class AddOutcome {
    kAdded,      // a new session
    kCompleted,  // a cleared session given back what it had lost
    kSkipped,    // already stored, nothing written
};

// Audio is durable within a second and exists to resume a crash: Finalise
// seals the transcript and erases audio, Cancel retains nothing, anything
// else is a crash and stays discoverable
class ISessionStore {
   public:
    virtual ~ISessionStore() = default;

    virtual SessionId Begin(const SessionMeta& meta) = 0;

    // lost_frames counts audio that belonged before these frames but never
    // arrived, mirroring the audio port
    virtual void Append(const SessionId& id, std::span<const float> frames,
                        std::uint64_t lost_frames) = 0;

    // The transcript is written at finalise, in one transaction before the
    // session seals
    virtual void ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) = 0;

    virtual void Finalise(const SessionId& id) = 0;
    virtual void Cancel(const SessionId& id) = 0;

    virtual void Abandon(const SessionId& id) = 0;

    virtual std::vector<SessionSummary> ListSessions() = 0;

    // Save records a generation, Edit the clinician's text over it. Reads of
    // an unknown session throw
    virtual void SaveDocument(const SessionId& id, DocumentKind kind, const Document& document) = 0;
    virtual void EditDocument(const SessionId& id, DocumentKind kind, const std::string& text) = 0;
    virtual Document ReadDocument(const SessionId& id, DocumentKind kind) = 0;

    // Removes one kind. A kind the session never had is not an error
    virtual void DeleteDocument(const SessionId& id, DocumentKind kind) = 0;

    // Read-back and disposal. All refuse the session currently recording
    virtual std::vector<asr::Turn> ReadTurns(const SessionId& id) = 0;

    // The stored capture in order, the basis for resuming a crashed session
    virtual std::vector<float> ReadAudio(const SessionId& id) = 0;

    virtual void Delete(const SessionId& id) = 0;

    // Erases every finalised session recorded with retain off. A crashed one
    // waits for its recovery, so the audio is never lost to the setting
    virtual void EraseUnretained() = 0;

    // Seeded samples carry demo. ClearDemo removes only those
    virtual SessionId Seed(const SessionSeed& seed) = 0;
    virtual std::size_t ClearDemo() = 0;

    // Crypto-erases every stored session except one still recording. With keep_reflections a
    // session holding an appraisal entry is cleared instead of erased. Returns the count
    virtual std::size_t DeleteAll(bool keep_reflections = false) = 0;

    // Erases everything a finalised session holds except its reflection, summary and label, so
    // the appraisal entry outlives the consultation. A session without one is deleted
    virtual void Clear(const SessionId& id) = 0;

    // A finalised session whole, for a backup. Throws for an unknown or recording session
    virtual SessionRecord ReadRecord(const SessionId& id) = 0;

    // Adds a session from a backup in one transaction under a fresh key, finalised, kept and
    // never demo, with its documents' timestamps and revisions as given. A cleared session with
    // the id gets back its transcript and the documents it lacks, keeping its own; any other
    // stored id is skipped with nothing written. Throws for a record ValidRecord refuses
    virtual AddOutcome AddRecord(const SessionRecord& record) = 0;

    // Called off the caller's thread when an audio commit fails. The store
    // keeps recording and retries
    virtual void SetFaultListener(std::function<void(const StoreError&)>) {}
};

}  // namespace clinicavt::store
