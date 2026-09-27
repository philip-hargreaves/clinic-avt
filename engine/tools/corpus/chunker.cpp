#include "tools/corpus/chunker.hpp"

#include <cctype>
#include <string_view>

#include "core/common/strings.hpp"
#include "core/guidance/guidance_query.hpp"

namespace clinicavt::guidance {
namespace {

std::vector<std::string> Paragraphs(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&] {
        const auto para = strings::Squeeze(current);
        if (strings::WordCount(para) >= kMinParagraphWords) out.push_back(para);
        current.clear();
    };
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            // a blank line (newline, optional spaces, newline) ends the paragraph
            std::size_t j = i + 1;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r')) ++j;
            if (j < text.size() && text[j] == '\n') {
                flush();
                i = j + 1;
                continue;
            }
        }
        current.push_back(text[i]);
        ++i;
    }
    flush();
    return out;
}

// A string field that may be absent or null
std::string Str(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool IncludeDocument(const nlohmann::json& entry, const std::string& code,
                     const std::set<std::string>& requested_codes) {
    if (!requested_codes.count(code)) return false;
    const auto stub = entry.find("is_stub");
    if (stub != entry.end() && stub->is_boolean() && stub->get<bool>()) return false;
    return Str(entry, "status") == "current";
}

std::vector<Chunk> ChunksFromDocument(const nlohmann::json& doc, std::set<std::string>& seen) {
    std::vector<Chunk> out;
    const auto code = doc.at("code").get<std::string>();
    const auto title = Str(doc, "title");
    const auto source_url = Str(doc, "source_url");
    const auto last_updated = Str(doc, "last_updated");
    for (const auto& chapter : doc.at("chapters")) {
        const auto chapter_title = Str(chapter, "title");
        const auto slug = Str(chapter, "slug");
        for (const auto& rec : chapter.at("recommendations")) {
            if (Str(rec, "kind") != "recommendation") continue;
            const auto id = rec.at("id").get<std::string>();
            if (!seen.insert(id).second) continue;
            Chunk chunk;
            chunk.id = id;
            chunk.code = code;
            chunk.title = title;
            chunk.chapter = chapter_title;
            chunk.number = Str(rec, "number");
            chunk.section = Str(rec, "section");
            chunk.update_tag = Str(rec, "update_tag");
            chunk.last_updated = last_updated;
            chunk.text = std::string(strings::Trim(Str(rec, "text")));
            chunk.url = source_url + "/chapter/" + slug + "#" + id;
            out.push_back(std::move(chunk));
        }
    }
    return out;
}

std::vector<Chunk> ChunksFromText(const std::string& code, const std::string& title,
                                  const std::string& text, const std::string& url) {
    std::vector<Chunk> out;
    std::vector<std::string> buffer;
    int buffered = 0;
    std::string number;
    auto close = [&] {
        if (buffer.empty()) return;
        Chunk chunk;
        chunk.id = code + "-" + std::to_string(out.size() + 1);
        chunk.code = code;
        chunk.title = title;
        chunk.number = number;
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            if (i) chunk.text += ' ';
            chunk.text += buffer[i];
        }
        chunk.url = url;
        out.push_back(std::move(chunk));
        buffer.clear();
        buffered = 0;
        number.clear();
    };
    for (const auto& para : Paragraphs(text)) {
        const int words = strings::WordCount(para);
        const bool starts = StartsRecommendation(para);
        if (!buffer.empty() &&
            (starts || buffered + words > kMaxWords || buffered >= kTargetWords)) {
            close();
        }
        if (buffer.empty()) number = LeadingNumber(para);
        buffer.push_back(para);
        buffered += words;
    }
    close();
    return out;
}

}  // namespace clinicavt::guidance
