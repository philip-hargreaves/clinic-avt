#pragma once

namespace clinicavt::system {

// Multithreaded COM on this thread for the object's lifetime. `hr` is the init result
struct ComApartment {
    long hr;  // HRESULT
    ComApartment();
    ~ComApartment();
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

}  // namespace clinicavt::system
