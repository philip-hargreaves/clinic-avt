#pragma once

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <tlhelp32.h>
// clang-format on

namespace clinicavt::system {

struct ProcessEntry {
    DWORD pid = 0;
    DWORD parent = 0;
    std::wstring image;
    std::uint64_t created = 0;  // FILETIME ticks, 0 when the process cannot be opened
};

// Live processes only: one that has exited is absent even while a handle to
// it is still open
inline std::vector<ProcessEntry> ListProcesses() {
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

inline std::uint64_t CreationTime(DWORD pid) {
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

// The processes named `image` whose parent is gone: absent, or a newer
// process now holding its pid. A creation time that cannot be read counts as
// a live parent
inline std::vector<DWORD> Orphans(const std::vector<ProcessEntry>& entries, const wchar_t* image) {
    std::vector<DWORD> orphans;
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

// Live processes named `image` whose engine has gone. A note host outlives its
// engine only while it tears down, or for good when stuck in the driver
inline std::vector<DWORD> OrphanedProcesses(const wchar_t* image) {
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

// The orphans still there after `grace`, which a host tearing down never is
inline std::vector<DWORD> LingeringOrphans(const wchar_t* image, std::chrono::milliseconds grace) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    for (;;) {
        auto orphans = OrphanedProcesses(image);
        if (orphans.empty() || std::chrono::steady_clock::now() >= deadline) return orphans;
        Sleep(200);
    }
}

}  // namespace clinicavt::system
