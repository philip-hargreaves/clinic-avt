#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

#include "adapters/interfaces/guidance_retriever.hpp"

namespace clinicavt::guidance {

using nlohmann::json;

// Bump when a field changes meaning. Readers refuse newer records
inline constexpr int kRecordVersion = 1;

// The wire and the store use the same shape. guidance/ready adds the session id
json ToJson(const Corpus& corpus);
json ToJson(const Results& results);

Results FromJson(const json& j);

// A search kept with its session, tied to the note revision it ran on
struct Record {
    Results results;
    std::int64_t note_revision = 0;
};

json ToJson(const Record& record);
Record RecordFromJson(const json& j);

// Store text, with invalid UTF-8 replaced as on the wire
std::string Dump(const Record& record);

// True for an object whose version, if present, is 1 to kRecordVersion
bool CanRead(const json& j);

}  // namespace clinicavt::guidance
