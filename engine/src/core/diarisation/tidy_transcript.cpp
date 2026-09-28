#include "core/diarisation/tidy_transcript.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common/strings.hpp"
#include "ports/audio_source.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::diar {

namespace {

std::string NormalisedWord(std::string_view word) {
    std::string out;
    for (const char c : word) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 || c == '\'') out.push_back(static_cast<char>(std::tolower(u)));
    }
    return out;
}

bool IsDisfluency(const std::string& w) {
    static const char* const kWords[] = {"um", "uh",  "er",  "erm", "hm", "hmm",
                                         "mm", "mmm", "mhm", "ah",  "eh", "huh"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

bool IsFunctionWord(const std::string& w) {
    static const char* const kWords[] = {"and", "so", "but", "or", "the", "a",
                                         "an",  "to", "of",  "in", "i",   "it"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

std::string Capitalised(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c) != 0) {
            text[i] = static_cast<char>(std::toupper(c));
            break;
        }
        if (std::isdigit(c) != 0) break;
    }
    // standalone i / i'm / i've / i'll / i'd
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != 'i') continue;
        const bool start = i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])) != 0;
        const bool end = i + 1 == text.size() ||
                         std::isspace(static_cast<unsigned char>(text[i + 1])) != 0 ||
                         text[i + 1] == '\'' || text[i + 1] == ',' || text[i + 1] == '.';
        if (start && end) text[i] = 'I';
    }
    return text;
}

// Any non-ASCII byte counts as a letter
bool HasWordCharacter(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u > 127 || std::isalnum(u) != 0;
    });
}

std::string Terminated(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.pop_back();
    }
    if (text.empty() || strings::EndsSentence(text)) return text;
    if (text.back() == ',' || text.back() == ';' || text.back() == ':') text.pop_back();
    if (!text.empty() && std::isalnum(static_cast<unsigned char>(text.back())) != 0) {
        text.push_back('.');
    }
    return text;
}

}  // namespace

bool NoContent(const std::string& text) {
    std::size_t function_words = 0;
    for (const auto raw : strings::Words(text)) {
        const auto w = NormalisedWord(raw);
        if (w.empty() || IsDisfluency(w)) continue;
        if (IsFunctionWord(w)) {
            ++function_words;
            continue;
        }
        return false;
    }
    // Also true with no words at all: punctuation left by a clipped decode
    return function_words <= 2;
}

std::vector<asr::Turn> TidyTranscript(std::vector<asr::Turn> turns) {
    std::vector<asr::Turn> merged;
    for (auto& turn : turns) {
        if (!HasWordCharacter(turn.text)) continue;
        if (!merged.empty() && merged.back().speaker == turn.speaker) {
            auto& prev = merged.back();
            const std::uint64_t prev_end = prev.first_frame + prev.frame_count;
            const std::uint64_t gap = turn.first_frame > prev_end ? turn.first_frame - prev_end : 0;
            if (gap <= kTidyMergeGapFrames) {
                // A fragment after a finished sentence starts the next one. After
                // a trail-off ("So I...") it continues the same one
                const bool trail_off =
                    prev.text.size() >= 3 && prev.text.compare(prev.text.size() - 3, 3, "...") == 0;
                prev.text += ' ';
                prev.text += strings::EndsSentence(prev.text) && !trail_off ? Capitalised(turn.text)
                                                                            : turn.text;
                const std::uint64_t end = turn.first_frame + turn.frame_count;
                if (end > prev_end) prev.frame_count = end - prev.first_frame;
                continue;
            }
        }
        merged.push_back(std::move(turn));
    }
    std::vector<asr::Turn> out;
    for (auto& turn : merged) {
        if (NoContent(turn.text)) continue;
        turn.text = Terminated(Capitalised(std::move(turn.text)));
        out.push_back(std::move(turn));
    }
    return out;
}

}  // namespace clinicavt::diar
