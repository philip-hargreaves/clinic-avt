#pragma once

#include <stdexcept>
#include <string>

// Load failure with a reason, used by the corpus and the index
namespace clinicavt::guidance {

struct Refused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void Guard(bool ok, const std::string& why) {
    if (!ok) throw Refused(why);
}

}  // namespace clinicavt::guidance
