#pragma once

namespace clinicavt::system {

// Blocks standby while alive: sleeping mid-load or mid-generation can leave GPU
// work the driver never finishes. Best effort
class AwakeRequest {
   public:
    explicit AwakeRequest(const wchar_t* reason);
    ~AwakeRequest();

    AwakeRequest(const AwakeRequest&) = delete;
    AwakeRequest& operator=(const AwakeRequest&) = delete;

   private:
    void* request_;  // HANDLE
};

}  // namespace clinicavt::system
