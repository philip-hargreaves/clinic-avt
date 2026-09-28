#include "core/guidance/patient_screen.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace clinicavt::guidance {
namespace {

TEST(PatientScreen, FindsNhsNumbersLettersAndRecordsAndNothingInAGuideline) {
    const std::pair<const char*, bool> rows[] = {
        {"Patient 943 476 5919 attended today.", true},
        {"ref 9434765919", true},
        {"ref 9434765918", false},  // the check digit fails
        // Ten digits spaced as a phone number, the check digit holding by chance
        {"a nurse led Helpline (0808 800035).", false},
        {"ISBN 978 0 19 923 5", false},
        {"GRADE 1A, SOA 100%, 2017, doi 10.1093/rheumatology/kex250", false},
        {"Dear Dr Smith, thank you for seeing this man.", true},
        {"DISCHARGE SUMMARY\nWard 4", true},
        {"DOB: 12/03/1961", true},
        // The app's own exports are refused if filed as guidance
        {"ClinicAVT export - not for the guidelines folder.\n\nPlan: ...", true},
        {"We recommend initiation of low-dose steroid therapy with gradually tailored tapering in "
         "straightforward PMR (B). Daily prednisolone 15 mg for 3 weeks.",
         false},
    };
    for (const auto& [text, patient] : rows) EXPECT_EQ(LooksLikePatientData(text), patient) << text;
}

}  // namespace
}  // namespace clinicavt::guidance
