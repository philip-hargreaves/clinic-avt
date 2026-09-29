#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/common/strings.hpp"

namespace clinicavt::note {

// Note model's first line for a non-consultation transcript; reported as a refusal, not saved
inline constexpr std::string_view kNotAConsultation = "NOT A CONSULTATION:";

// The reason after the sentinel when the note is a refusal, else nothing
inline std::optional<std::string> RefusalReason(std::string_view note) {
    const auto text = strings::Trim(note);
    if (text.substr(0, kNotAConsultation.size()) != kNotAConsultation) return std::nullopt;
    auto reason = text.substr(kNotAConsultation.size());
    if (const auto line_end = reason.find('\n'); line_end != std::string_view::npos) {
        reason = reason.substr(0, line_end);
    }
    return std::string(strings::Trim(reason));
}

// Buffers streamed text until it can't be a refusal, so a refusal is never shown as a note
class RefusalFilter {
   public:
    explicit RefusalFilter(std::function<void(const std::string&)> forward)
        : forward_(std::move(forward)) {}

    // Cumulative text so far, as the writer streams it
    void operator()(const std::string& text) {
        if (refused_) return;
        if (!decided_) {
            const auto opening = strings::Trim(text);
            const auto n = std::min(opening.size(), kNotAConsultation.size());
            if (opening.substr(0, n) == kNotAConsultation.substr(0, n)) {
                if (opening.size() < kNotAConsultation.size()) return;  // still could be
                refused_ = true;
                return;
            }
            decided_ = true;
        }
        forward_(text);
    }

    bool Refused() const {
        return refused_;
    }

   private:
    std::function<void(const std::string&)> forward_;
    bool decided_ = false;
    bool refused_ = false;
};

}  // namespace clinicavt::note
