#include "core/guidance/patient_screen.hpp"

#include <cctype>
#include <string>
#include <string_view>

#include "core/common/strings.hpp"

namespace clinicavt::guidance {

namespace {

// Ten digits, unspaced or grouped 3-3-4 as the NHS writes them, whose check
// digit holds. A helpline number spaced otherwise is left alone
bool HoldsNhsNumber(std::string_view text) {
    int digits[10];
    int count = 0;
    const auto valid = [&] {
        if (count != 10) return false;
        int sum = 0;
        for (int d = 0; d < 9; ++d) sum += digits[d] * (10 - d);
        const int check = (11 - sum % 11) % 11;
        return check != 10 && check == digits[9];
    };
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            if (count < 10) digits[count] = c - '0';
            ++count;
        } else if (c != ' ' || (count != 3 && count != 6)) {
            if (valid()) return true;
            count = 0;
        }
    }
    return valid();
}

}  // namespace

bool LooksLikePatientData(std::string_view text) {
    const std::string lower = strings::Lower(text);
    static const char* const kPhrases[] = {
        "date of birth", "dob:", "nhs number", "nhs no", "dear dr", "dear doctor",
        "discharge summary", "discharge letter",
        // The app marks its own note and sheet exports, so a filed one is refused
        "clinicavt export"};
    for (const char* phrase : kPhrases) {
        if (strings::Contains(lower, phrase)) return true;
    }
    return HoldsNhsNumber(text);
}

}  // namespace clinicavt::guidance
