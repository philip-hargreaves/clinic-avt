#pragma once

#include <optional>
#include <string>
#include <vector>

namespace clinicavt::records {

// A guideline or document the clinician ticked. Stores its own copy of the text
// so it still reads after the source is gone
struct Reference {
    std::string key;
    std::string reference;
    std::string title;
    std::string link;
    std::string source;
};

// The appraisal answers and ticked references, stored as one sealed text
struct Answers {
    std::string happened;
    std::string learned;
    std::string next;
    // Always set by Decode. When unset, Encode leaves it out
    std::optional<std::vector<Reference>> references;
};

// Converts a reflection to and from its stored text
class IReflectionCodec {
   public:
    virtual ~IReflectionCodec() = default;
    // Text that does not parse, or a field of the wrong type, reads as empty
    virtual Answers Decode(const std::string& text) const = 0;
    virtual std::string Encode(const Answers& answers) const = 0;
};

}  // namespace clinicavt::records
