#include "adapters/storage/reflection_json.hpp"

#include <nlohmann/json.hpp>

namespace clinicavt::store {

namespace {

using json = nlohmann::json;

std::string StringField(const json& object, const char* key) {
    return object.is_object() && object.contains(key) && object[key].is_string()
               ? object[key].get<std::string>()
               : std::string();
}

}  // namespace

// Answers and ticked references are one sealed JSON text. Text that does not parse reads as empty
records::Answers JsonReflectionCodec::Decode(const std::string& text) const {
    const json parsed = json::parse(text, nullptr, false);
    records::Answers answers;
    answers.happened = StringField(parsed, "happened");
    answers.learned = StringField(parsed, "learned");
    answers.next = StringField(parsed, "next");
    answers.references.emplace();
    if (parsed.is_object() && parsed.contains("references") && parsed["references"].is_array()) {
        for (const auto& given : parsed["references"]) {
            answers.references->push_back({StringField(given, "key"),
                                           StringField(given, "reference"),
                                           StringField(given, "title"), StringField(given, "link"),
                                           StringField(given, "source")});
        }
    }
    return answers;
}

std::string JsonReflectionCodec::Encode(const records::Answers& answers) const {
    json encoded{
        {"happened", answers.happened}, {"learned", answers.learned}, {"next", answers.next}};
    if (answers.references) {
        encoded["references"] = json::array();
        for (const auto& reference : *answers.references) {
            encoded["references"].push_back({{"key", reference.key},
                                             {"reference", reference.reference},
                                             {"title", reference.title},
                                             {"link", reference.link},
                                             {"source", reference.source}});
        }
    }
    return encoded.dump();
}

}  // namespace clinicavt::store
