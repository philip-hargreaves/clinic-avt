#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include "adapters/system/power_throttling.hpp"

namespace clinicavt::system {

// A Win32 handle closed with its scope
class UniqueHandle {
   public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() {
        Reset();
    }
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) Reset(other.Release());
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const {
        return value_;
    }
    explicit operator bool() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE Release() {
        return std::exchange(value_, nullptr);
    }
    void Reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

   private:
    HANDLE value_ = nullptr;
};

struct ChildOptions {
    HANDLE stdin_read = nullptr;          // inherited as the child's stdin, null keeps the parent's
    HANDLE stdout_write = nullptr;        // likewise for stdout
    std::size_t memory_cap = 0;           // job memory limit in bytes, 0 for none
    bool single_process = false;          // the job admits one process
    bool exempt_from_throttling = false;  // EcoQoS off, as the engine itself runs
};

// A child process in a kill-on-close job, so it can never outlive the engine.
// Stderr is the parent's: the child's diagnostics land in the same log
class ChildProcess {
   public:
    ChildProcess() = default;

    // Spawns `exe` suspended, puts it in the job, then resumes it. Throws
    // std::runtime_error when it cannot start
    static ChildProcess Spawn(const std::filesystem::path& exe, const std::wstring& args,
                              const ChildOptions& options = {}) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput =
            options.stdin_read != nullptr ? options.stdin_read : GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = options.stdout_write != nullptr ? options.stdout_write
                                                             : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        std::wstring command = L"\"" + exe.wstring() + L"\" " + args;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe.wstring().c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                            &info)) {
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
            SetInformationJobObject(child.job_.get(), JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits));
            AssignProcessToJobObject(child.job_.get(), child.process_.get());
        }
        if (options.exempt_from_throttling) DisableThrottling(child.process_.get());
        ResumeThread(thread.get());
        return child;
    }

    bool Alive() const {
        return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
    }

    DWORD Pid() const {
        return process_ ? GetProcessId(process_.get()) : 0;
    }

    // True once the process has ended, waiting up to `ms` for it
    bool WaitFor(DWORD ms) const {
        return process_ && WaitForSingleObject(process_.get(), ms) != WAIT_TIMEOUT;
    }

    // The whole job when there is one, else the process alone
    void Kill() {
        if (job_) {
            TerminateJobObject(job_.get(), 1);
        } else if (process_) {
            TerminateProcess(process_.get(), 1);
        }
    }

    DWORD ExitCode() const {
        DWORD code = 0;
        if (process_) GetExitCodeProcess(process_.get(), &code);
        return code;
    }

    // Waits up to `grace_ms` for the child to exit on its own, then lets the
    // handles go. False keeps a child that is still running held, because
    // closing the job would kill it
    bool End(DWORD grace_ms) {
        if (process_ && !WaitFor(grace_ms)) return false;
        process_.Reset();
        job_.Reset();
        return true;
    }

   private:
    UniqueHandle process_;
    UniqueHandle job_;
};

}  // namespace clinicavt::system
