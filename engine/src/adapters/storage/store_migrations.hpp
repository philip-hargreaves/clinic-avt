#pragma once

#include <cstdint>
#include <filesystem>

#include "adapters/storage/db.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::store {

inline constexpr std::int64_t kSchemaVersion = 7;
// Stores older than this cannot be upgraded
inline constexpr std::int64_t kOldestSchemaVersion = 6;
// "AMBC" header mark for a clinical store
inline constexpr std::int64_t kApplicationId = 0x414D4243;

// A store this build cannot open because of its schema version
class StoreVersionError : public StoreError {
   public:
    explicit StoreVersionError(bool newer)
        : StoreError(StoreCode::kSchema, newer ? "store schema is newer than this build"
                                               : "store schema is too old for this build"),
          newer_(newer) {}

    bool Newer() const {
        return newer_;
    }

   private:
    bool newer_;
};

// Opens or creates the database under root and upgrades an older schema one version at a time,
// each step and its version stamp in one transaction. Throws StoreVersionError for a version
// outside kOldestSchemaVersion..kSchemaVersion and StoreError for a file that is not a store
Db OpenDatabase(const std::filesystem::path& root);

}  // namespace clinicavt::store
