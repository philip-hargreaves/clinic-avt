#include "adapters/system/process_scan.hpp"

#include <cwchar>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// clang-format off
#include <tlhelp32.h>
// clang-format on

namespace clinicavt::system {

std::vector<ProcessEntry> ListProcesses() {
    std::vector<ProcessEntry> entries;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return entries;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        entries.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile, 0});
    }
    CloseHandle(snapshot);
    return entries;
}

std::uint64_t CreationTime(ProcessId pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    std::uint64_t ticks = 0;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    CloseHandle(process);
    return ticks;
}

std::vector<ProcessId> Orphans(const std::vector<ProcessEntry>& entries, const wchar_t* image) {
    std::vector<ProcessId> orphans;
    for (const auto& child : entries) {
        if (_wcsicmp(child.image.c_str(), image) != 0) continue;
        const ProcessEntry* parent = nullptr;
        for (const auto& candidate : entries) {
            if (candidate.pid == child.parent) parent = &candidate;
        }
        const bool reused = parent != nullptr && parent->created != 0 && child.created != 0 &&
                            parent->created > child.created;
        if (parent == nullptr || reused) orphans.push_back(child.pid);
    }
    return orphans;
}

std::vector<ProcessId> OrphanedProcesses(const wchar_t* image) {
    auto entries = ListProcesses();
    for (auto& entry : entries) {
        if (_wcsicmp(entry.image.c_str(), image) != 0) continue;
        entry.created = CreationTime(entry.pid);
        for (auto& parent : entries) {
            if (parent.pid == entry.parent && parent.created == 0) {
                parent.created = CreationTime(parent.pid);
            }
        }
    }
    return Orphans(entries, image);
}

std::vector<ProcessId> LingeringOrphans(const wchar_t* image, std::chrono::milliseconds grace) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    for (;;) {
        auto orphans = OrphanedProcesses(image);
        if (orphans.empty() || std::chrono::steady_clock::now() >= deadline) return orphans;
        Sleep(200);
    }
}

}  // namespace clinicavt::system
