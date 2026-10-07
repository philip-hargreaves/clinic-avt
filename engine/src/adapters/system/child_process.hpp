#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace clinicavt::system {

// Owns a Win32 HANDLE, held as void* so windows.h stays out of the header
class UniqueHandle {
   public:
    UniqueHandle() = default;
    explicit UniqueHandle(void* value) : value_(value) {}
    ~UniqueHandle();
    UniqueHandle(UniqueHandle&& other) noexcept;
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    void* Get() const {
        return value_;
    }
    explicit operator bool() const;
    void* Release();
    void Reset(void* value = nullptr);

   private:
    void* value_ = nullptr;
};

struct ChildOptions {
    void* stdin_read = nullptr;           // inherited as the child's stdin, null keeps the parent's
    void* stdout_write = nullptr;         // likewise for stdout
    std::size_t memory_cap = 0;           // job memory limit in bytes, 0 for none
    bool single_process = false;          // the job admits one process
    bool exempt_from_throttling = false;  // EcoQoS off, like the engine
};

// Child in a kill-on-close job so it never outlives the engine. Shares the
// parent's stderr so its logs go to the same place
class ChildProcess {
   public:
    ChildProcess() = default;

    // Spawns `exe` suspended, puts it in the job, then resumes it. Throws
    // std::runtime_error when it cannot start
    static ChildProcess Spawn(const std::filesystem::path& exe, const std::wstring& args,
                              const ChildOptions& options = {});

    bool Alive() const;

    unsigned long Pid() const;

    // True once the process has ended, waiting up to `ms` for it
    bool WaitFor(unsigned long ms) const;

    // Kills the whole job if any, else the process
    void Kill();

    unsigned long ExitCode() const;

    // Waits up to grace_ms for exit, then releases the handles. Returns false and
    // keeps the handles if still running, since closing the job would kill it
    bool End(unsigned long grace_ms);

   private:
    UniqueHandle process_;
    UniqueHandle job_;
};

}  // namespace clinicavt::system
