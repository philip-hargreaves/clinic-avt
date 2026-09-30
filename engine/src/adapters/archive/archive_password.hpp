#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace clinicavt::archive {

// Fewer code points than this and a backup is refused
inline constexpr std::size_t kMinPasswordLength = 8;

// The bytes a backup key is derived from: the UTF-8 of the password's NFC form, nothing trimmed,
// so an accent typed composed on one keyboard and decomposed on another opens the same file
std::string KdfInput(std::string_view password);

std::size_t CodePoints(std::string_view utf8);

void Wipe(std::string& secret);
void Wipe(std::wstring& secret);

// Wipes the secret on scope exit, including on throw
template <typename Secret>
struct WipeOnExit {
    Secret& secret;
    ~WipeOnExit() {
        Wipe(secret);
    }
};

}  // namespace clinicavt::archive
