#pragma once

#include <exception>

#include "core/common/log.hpp"
#include "ports/session_events.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::session {

// Logs every store failure and raises OnStorageFault for disk-full or I/O errors
inline void ReportStoreFailure(ICaptureEvents& events, const char* what, const std::exception& e) {
    log::Printf("clinicavt-engine: store %s failed: %s\n", what, e.what());
    const auto* fault = dynamic_cast<const store::StoreError*>(&e);
    if (fault != nullptr &&
        (fault->Code() == store::StoreCode::kFull || fault->Code() == store::StoreCode::kIo)) {
        events.OnStorageFault(e.what());
    }
}

}  // namespace clinicavt::session
