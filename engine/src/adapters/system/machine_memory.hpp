#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>

namespace clinicavt::system {

inline std::optional<std::uint64_t> InstalledMemoryBytes() {
    ULONGLONG kilobytes = 0;
    if (!GetPhysicallyInstalledSystemMemory(&kilobytes)) return std::nullopt;
    return static_cast<std::uint64_t>(kilobytes) * 1024;
}

// What the Intel GPU may use, as Windows reports it: its own memory plus its
// share of RAM. Shared memory overrides in the driver show here
inline std::optional<std::uint64_t> IntelGpuMemoryBytes() {
    constexpr UINT kIntel = 0x8086;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.VendorId != kIntel || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        return static_cast<std::uint64_t>(desc.DedicatedVideoMemory) + desc.SharedSystemMemory;
    }
    return std::nullopt;
}

}  // namespace clinicavt::system
