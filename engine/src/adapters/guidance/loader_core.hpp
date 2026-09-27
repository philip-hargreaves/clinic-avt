#pragma once

#include <stdexcept>
#include <string>

// A refusal with its reason, for the corpus and the index when they load
namespace clinicavt::guidance {

struct Refused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void Guard(bool ok, const std::string& why) {
    if (!ok) throw Refused(why);
}

}  // namespace clinicavt::guidance
