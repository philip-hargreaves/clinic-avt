#include "adapters/ipc/pipe_client.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>

namespace clinicavt::ipc {

PipeClient::PipeClient() : pipe_(INVALID_HANDLE_VALUE) {}

PipeClient::~PipeClient() {
    Close();
}

bool PipeClient::Open(const std::wstring& path) {
    Close();
    pipe_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    return IsOpen();
}

bool PipeClient::IsOpen() const {
    return pipe_ != INVALID_HANDLE_VALUE;
}

void PipeClient::Close() {
    if (IsOpen()) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    decoder_ = FrameDecoder{};
}

unsigned long PipeClient::ServerPid() const {
    ULONG pid = 0;
    if (!IsOpen() || !GetNamedPipeServerProcessId(pipe_, &pid)) return 0;
    return pid;
}

bool PipeClient::Write(std::string_view frame) {
    DWORD written = 0;
    return IsOpen() &&
           WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) &&
           written == frame.size();
}

PipeClient::Poll PipeClient::Read(std::size_t max) {
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

}  // namespace clinicavt::ipc
