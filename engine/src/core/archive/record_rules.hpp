#pragma once

#include <string>

#include "ports/session_store.hpp"

namespace clinicavt::archive {

// Exactly the form Iso8601 writes, since the stores order timestamps as text
bool IsIso8601(const std::string& text);

// A record the store could have written itself: a session id, times in the store's one form, a
// positive sample rate, known kinds once each and revisions with headroom. Restore checks every
// record with this before writing any, and AddRecord refuses the same
bool ValidRecord(const store::SessionRecord& record);

}  // namespace clinicavt::archive
