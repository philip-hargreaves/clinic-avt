#include "adapters/system/com_apartment.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on

namespace clinicavt::system {

ComApartment::ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

ComApartment::~ComApartment() {
    if (SUCCEEDED(hr)) CoUninitialize();
}

}  // namespace clinicavt::system
