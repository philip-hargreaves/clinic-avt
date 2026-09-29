#pragma once

#include <stdexcept>
#include <string>

namespace clinicavt::store {

// Lets callers tell, e.g., a missing session from a full disk
enum class StoreCode { kNotFound, kBusy, kFull, kIo, kAuth, kSchema, kOther };

class StoreError : public std::runtime_error {
   public:
    StoreError(StoreCode code, const std::string& what) : std::runtime_error(what), code_(code) {}

    StoreCode Code() const {
        return code_;
    }

   private:
    StoreCode code_;
};

}  // namespace clinicavt::store
