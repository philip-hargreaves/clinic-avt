#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "adapters/ipc/framing.hpp"

namespace clinicavt::ipc {

// Client end of a private named pipe. Synchronous, so callers serialise reads
class PipeClient {
   public:
    PipeClient();
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // False if nothing serves `path` yet
    bool Open(const std::wstring& path);

    bool IsOpen() const;

    void Close();

    // Server process id, or 0 if unknown
    unsigned long ServerPid() const;

    // False when the pipe is gone
    bool Write(std::string_view frame);

    enum class Poll { kNothing, kRead, kGone };

    // Non-blocking read of up to `max` bytes into the decoder
    Poll Read(std::size_t max = std::size_t{64} * 1024);

    std::optional<std::string> NextFrame() {
        return decoder_.Next();
    }

   private:
    void* pipe_;  // HANDLE
    FrameDecoder decoder_;
};

}  // namespace clinicavt::ipc
