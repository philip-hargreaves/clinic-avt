#include "core/session/review.hpp"

#include <mutex>
#include <utility>
#include <vector>

namespace clinicavt::session {

Review::Review(SessionState& state, NoteLane& note_lane, store::IDocumentStore& documents,
               store::ISessionCatalog& catalog)
    : state_(state), note_lane_(note_lane), documents_(documents), catalog_(catalog) {}

bool Review::Open(const store::SessionId& id) {
    if (state_.Running() || note_lane_.Busy()) {
        return false;
    }
    try {
        (void)catalog_.ReadTurns(id);
    } catch (...) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.last_finalised = id;
    state_.reviewing = true;
    note_lane_.ClearRefusal();
    return true;
}

void Review::Close() {
    store::SessionId refused;
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (note_lane_.Refused()) {
            // Delete only a fresh capture whose note was refused. A reviewed session is kept
            if (!state_.reviewing) refused = std::exchange(state_.last_finalised, {});
            note_lane_.ClearRefusal();
        }
        if (state_.reviewing) {
            state_.reviewing = false;
            state_.last_finalised.clear();
        }
    }
    if (!refused.empty()) {
        try {
            catalog_.Delete(refused);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the session stays, as if kept
        }
    }
    if (!state_.Running()) {
        try {
            catalog_.EraseUnretained();
        } catch (...) {  // NOLINT(bugprone-empty-catch) retried at the next start
        }
    }
}

bool Review::RegenerateNote(note::NoteOptions options) {
    if (!note_lane_.Available() || state_.Running() || note_lane_.Busy()) {
        return false;
    }
    store::SessionId id = state_.LastFinalised();
    if (id.empty()) {
        return false;
    }
    std::vector<asr::Turn> turns;
    try {
        turns = catalog_.ReadTurns(id);
    } catch (...) {
        return false;
    }
    // A session restored without its transcript has nothing to write from
    if (turns.empty()) {
        return false;
    }
    note_lane_.SetOptions(options);
    note_lane_.WriteNote(std::move(id), std::move(turns));
    return true;
}

bool Review::WriteSummary(store::SessionId id) {
    if (!note_lane_.Available() || state_.Running() || note_lane_.Busy()) {
        return false;
    }
    std::string note = StoredNote(id);
    if (note.empty()) {
        return false;
    }
    note_lane_.WriteSummary(std::move(id), std::move(note));
    return true;
}

bool Review::RegeneratePatient() {
    if (!note_lane_.WritesPatient() || state_.Running() || note_lane_.Busy()) {
        return false;
    }
    store::SessionId id = state_.LastFinalised();
    if (id.empty()) {
        return false;
    }
    std::string note = StoredNote(id);
    if (note.empty()) {
        return false;
    }
    note_lane_.WritePatient(std::move(id), std::move(note));
    return true;
}

// Empty when the session has no note or cannot be read
std::string Review::StoredNote(const store::SessionId& id) const {
    try {
        return documents_.ReadDocument(id, store::DocumentKind::kNote).text;
    } catch (...) {
        return {};
    }
}

}  // namespace clinicavt::session
