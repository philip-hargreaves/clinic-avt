#pragma once

#include <string>

#include "ports/reflection_codec.hpp"

namespace clinicavt::store {

// Stores a reflection as one JSON object, with the answers as strings and the references as an
// array of objects
class JsonReflectionCodec final : public records::IReflectionCodec {
   public:
    records::Answers Decode(const std::string& text) const override;
    std::string Encode(const records::Answers& answers) const override;
};

}  // namespace clinicavt::store
