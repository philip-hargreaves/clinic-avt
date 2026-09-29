#pragma once

#include <string_view>

namespace clinicavt::guidance {

// Text that looks like it is about a patient: an NHS number, a date of birth
// label, a letter's opening or a discharge heading. Guidelines carry none
bool LooksLikePatientData(std::string_view text);

}  // namespace clinicavt::guidance
