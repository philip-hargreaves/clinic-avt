#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace clinicavt::system {

// A Win32 process id (DWORD)
using ProcessId = unsigned long;

struct ProcessEntry {
    ProcessId pid = 0;
    ProcessId parent = 0;
    std::wstring image;
    std::uint64_t created = 0;  // FILETIME ticks, 0 when the process cannot be opened
};

// Running processes only; an exited one is absent even while a handle is open
std::vector<ProcessEntry> ListProcesses();

std::uint64_t CreationTime(ProcessId pid);

// Processes named `image` whose parent is gone (missing, or its pid reused by a
// newer process). An unreadable creation time counts as a live parent
std::vector<ProcessId> Orphans(const std::vector<ProcessEntry>& entries, const wchar_t* image);

// Processes named `image` whose engine is gone. A note host outlives its engine
// only while exiting, or indefinitely when stuck in the driver
std::vector<ProcessId> OrphanedProcesses(const wchar_t* image);

// Orphans still present after `grace`; an exiting host is gone by then
std::vector<ProcessId> LingeringOrphans(const wchar_t* image, std::chrono::milliseconds grace);

}  // namespace clinicavt::system
