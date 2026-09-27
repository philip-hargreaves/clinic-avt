#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl_core.h>
// clang-format on

#include <filesystem>

#include "adapters/system/com_apartment.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::system {

// Sends a file to the Recycle Bin, so a removed guideline is one click from
// coming back. Throws a store error when it cannot
inline void RecycleFile(const std::filesystem::path& path) {
    const ComApartment com;
    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
        throw store::StoreError(store::StoreCode::kOther, "the Recycle Bin is not available");
    }
    IShellItem* item = nullptr;
    const auto fail = [&](const char* what) {
        if (item != nullptr) item->Release();
        op->Release();
        throw store::StoreError(store::StoreCode::kBusy, what);
    };
    if (FAILED(op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
                                     FOF_NOERRORUI | FOFX_RECYCLEONDELETE))) {
        fail("the Recycle Bin is not available");
    }
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        fail("the file was not found");
    }
    if (FAILED(op->DeleteItem(item, nullptr))) fail("the file could not be removed");
    const HRESULT hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    item->Release();
    op->Release();
    if (FAILED(hr) || aborted) {
        throw store::StoreError(store::StoreCode::kBusy,
                                "the file could not be removed, it may be open in another program");
    }
}

}  // namespace clinicavt::system
