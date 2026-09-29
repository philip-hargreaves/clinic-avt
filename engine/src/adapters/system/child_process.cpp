#include "adapters/system/child_process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdexcept>
#include <utility>

#include "adapters/system/power_throttling.hpp"

namespace clinicavt::system {

UniqueHandle::~UniqueHandle() {
    Reset();
}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept : value_(other.Release()) {}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept {
    if (this != &other) Reset(other.Release());
    return *this;
}

UniqueHandle::operator bool() const {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
}

void* UniqueHandle::Release() {
    return std::exchange(value_, nullptr);
}

void UniqueHandle::Reset(void* value) {
    if (*this) CloseHandle(value_);
    value_ = value;
}

ChildProcess ChildProcess::Spawn(const std::filesystem::path& exe, const std::wstring& args,
                                 const ChildOptions& options) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput =
        options.stdin_read != nullptr ? options.stdin_read : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput =
        options.stdout_write != nullptr ? options.stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    std::wstring command = L"\"" + exe.wstring() + L"\" " + args;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.wstring().c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) {
        throw std::runtime_error(exe.filename().string() + " failed to start");
    }
    ChildProcess child;
    child.process_.Reset(info.hProcess);
    UniqueHandle thread(info.hThread);
    child.job_.Reset(CreateJobObjectW(nullptr, nullptr));
    if (child.job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (options.memory_cap > 0) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limits.ProcessMemoryLimit = options.memory_cap;
        }
        if (options.single_process) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
            limits.BasicLimitInformation.ActiveProcessLimit = 1;
        }
        SetInformationJobObject(child.job_.Get(), JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
        AssignProcessToJobObject(child.job_.Get(), child.process_.Get());
    }
    if (options.exempt_from_throttling) DisableThrottling(child.process_.Get());
    ResumeThread(thread.Get());
    return child;
}

bool ChildProcess::Alive() const {
    return process_ && WaitForSingleObject(process_.Get(), 0) == WAIT_TIMEOUT;
}

unsigned long ChildProcess::Pid() const {
    return process_ ? GetProcessId(process_.Get()) : 0;
}

bool ChildProcess::WaitFor(unsigned long ms) const {
    return process_ && WaitForSingleObject(process_.Get(), ms) != WAIT_TIMEOUT;
}

void ChildProcess::Kill() {
    if (job_) {
        TerminateJobObject(job_.Get(), 1);
    } else if (process_) {
        TerminateProcess(process_.Get(), 1);
    }
}

unsigned long ChildProcess::ExitCode() const {
    DWORD code = 0;
    if (process_) GetExitCodeProcess(process_.Get(), &code);
    return code;
}

bool ChildProcess::End(unsigned long grace_ms) {
    if (process_ && !WaitFor(grace_ms)) return false;
    process_.Reset();
    job_.Reset();
    return true;
}

}  // namespace clinicavt::system
