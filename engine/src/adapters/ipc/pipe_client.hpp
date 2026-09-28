#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "adapters/ipc/framing.hpp"

namespace clinicavt::ipc {

// Client end of a private named pipe. Synchronous; callers serialise reads
class PipeClient {
   public:
    PipeClient() = default;
    ~PipeClient() {
        Close();
    }
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // False if nothing serves `path` yet
    bool Open(const std::wstring& path) {
        Close();
        pipe_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        return IsOpen();
    }

    bool IsOpen() const {
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    void Close() {
        if (IsOpen()) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        decoder_ = FrameDecoder{};
    }

    // Server process id, or 0 if unknown
    DWORD ServerPid() const {
        ULONG pid = 0;
        if (!IsOpen() || !GetNamedPipeServerProcessId(pipe_, &pid)) return 0;
        return pid;
    }

    // False when the pipe is gone
    bool Write(std::string_view frame) {
        DWORD written = 0;
        return IsOpen() &&
               WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written,
                         nullptr) &&
               written == frame.size();
    }

    enum class Poll { kNothing, kRead, kGone };

    // Non-blocking read of up to `max` bytes into the decoder
    Poll Read(std::size_t max = std::size_t{64} * 1024) {
        DWORD available = 0;
        if (!IsOpen() || !PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
            return Poll::kGone;
        }
        if (available == 0) return Poll::kNothing;
        std::string buffer(std::min<std::size_t>(available, max), '\0');
        DWORD read = 0;
        if (!ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ||
            read == 0) {
            return Poll::kGone;
        }
        decoder_.Push({buffer.data(), read});
        return Poll::kRead;
    }

    std::optional<std::string> NextFrame() {
        return decoder_.Next();
    }

   private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    FrameDecoder decoder_;
};

}  // namespace clinicavt::ipc
