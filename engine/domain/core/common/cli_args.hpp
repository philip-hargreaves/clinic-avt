#pragma once

#include <string>
#include <vector>

namespace clinicavt {

// Removes "--name value" from args and returns the value, or "" if absent
inline std::string TakeFlag(std::vector<std::string>& args, const std::string& name) {
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (*it == name && std::next(it) != args.end()) {
            std::string value = *std::next(it);
            args.erase(it, it + 2);
            return value;
        }
    }
    return {};
}

// Removes a bare "--name" from args and returns whether it was present
inline bool TakeSwitch(std::vector<std::string>& args, const std::string& name) {
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (*it == name) {
            args.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace clinicavt
