#pragma once

#include <cstdint>
#include <filesystem>

#include "adapters/storage/db.hpp"

namespace clinicavt::store {

inline constexpr std::int64_t kSchemaVersion = 6;
// "AMBC" header mark for a clinical store
inline constexpr std::int64_t kApplicationId = 0x414D4243;

// Opens or creates the DB under `root`. Foreign files and other schema versions are refused,
// as there are no migrations
Db OpenDatabase(const std::filesystem::path& root);

}  // namespace clinicavt::store
