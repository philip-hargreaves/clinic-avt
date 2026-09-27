#pragma once

#include <cstdint>
#include <filesystem>

#include "adapters/storage/db.hpp"

namespace clinicavt::store {

inline constexpr std::int64_t kSchemaVersion = 6;
// "AMBC": the header mark of a clinical store
inline constexpr std::int64_t kApplicationId = 0x414D4243;

// Opens or creates the database under `root`. Foreign files and any other
// schema version are refused: there are no migrations
Db OpenDatabase(const std::filesystem::path& root);

}  // namespace clinicavt::store
