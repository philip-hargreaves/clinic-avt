#pragma once

#include <string>

#include "ports/session_store.hpp"

namespace clinicavt::archive {

// Exactly the form Iso8601 writes, since the stores order timestamps as text
bool IsIso8601(const std::string& text);

// True if the store could have written this record. Checked by Restore before any write
// and by AddRecord
bool ValidRecord(const store::SessionRecord& record);

}  // namespace clinicavt::archive
