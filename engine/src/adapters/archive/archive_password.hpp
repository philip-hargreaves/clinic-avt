#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace clinicavt::archive {

// Fewer code points than this and a backup is refused
inline constexpr std::size_t kMinPasswordLength = 12;

// The bytes a backup key is derived from: the UTF-8 of the password's NFC form, nothing trimmed,
// so an accent typed composed on one keyboard and decomposed on another opens the same file
std::string KdfInput(std::string_view password);

std::size_t CodePoints(std::string_view utf8);

// Overwrites a secret in place and empties it
void Wipe(std::string& secret);
void Wipe(std::wstring& secret);

// Wipes a secret when the scope ends, however it ends
struct WipeOnExit {
    std::string& secret;
    ~WipeOnExit() {
        Wipe(secret);
    }
};

}  // namespace clinicavt::archive
