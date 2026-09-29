#include "adapters/system/awake_request.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace clinicavt::system {

AwakeRequest::AwakeRequest(const wchar_t* reason) : request_(INVALID_HANDLE_VALUE) {
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = const_cast<wchar_t*>(reason);
    request_ = PowerCreateRequest(&context);
    if (request_ != INVALID_HANDLE_VALUE && request_ != nullptr) {
        PowerSetRequest(request_, PowerRequestSystemRequired);
    }
}

AwakeRequest::~AwakeRequest() {
    if (request_ != INVALID_HANDLE_VALUE && request_ != nullptr) {
        PowerClearRequest(request_, PowerRequestSystemRequired);
        CloseHandle(request_);
    }
}

}  // namespace clinicavt::system
