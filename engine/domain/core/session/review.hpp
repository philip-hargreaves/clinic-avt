#pragma once

#include <string>

#include "core/session/note_lane.hpp"
#include "core/session/session_state.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::session {

// Open and the note rewrites are refused while recording or writing, so the RPC thread never
// blocks on the note lane
class Review {
   public:
    Review(SessionState& state, NoteLane& note_lane, store::ISessionStore& store);

    bool Open(const store::SessionId& id);
    void Close();
    bool RegenerateNote(note::NoteOptions options);
    bool WriteSummary(store::SessionId id);
    bool RegeneratePatient();

   private:
    std::string StoredNote(const store::SessionId& id) const;

    SessionState& state_;
    NoteLane& note_lane_;
    store::ISessionStore& store_;
};

}  // namespace clinicavt::session
