#pragma once

#include <nlohmann/json.hpp>

#include "ports/session_archive.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::archive {

using nlohmann::json;

// What a backup's records hold. Every field is written. A reader refuses a missing field, a wrong
// type or an unknown document kind with kDamaged, and ignores fields it does not know
json ToJson(const Manifest& manifest);
Manifest ManifestFromJson(const json& j);

json ToJson(const store::SessionRecord& record);
store::SessionRecord RecordFromJson(const json& j);

}  // namespace clinicavt::archive
