#include "adapters/translate/nllb_translator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openvino/openvino.hpp>
#include <stdexcept>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/models/residency.hpp"
#include "core/common/log.hpp"
#include "core/common/strings.hpp"
#include "core/translate/plain_punctuation.hpp"

namespace clinicavt::translate {

namespace {

constexpr std::size_t kMaxTokens = 512;

// 0.9 GB while loaded, under 2 s to reload
constexpr auto kIdleRelease = std::chrono::minutes(10);

nlohmann::json LoadLanguages(const std::filesystem::path& dir) {
    std::ifstream in(dir / "languages.json");
    if (!in) {
        throw std::runtime_error("translation model has no languages.json");
    }
    return nlohmann::json::parse(in);
}

double SecondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

struct NllbTranslator::Impl {
    Impl(const models::ModelStore& model_store, models::OvRuntime& ov)
        : store(model_store), runtime(ov), info(store.Resolve("translation", "default")) {}

    const models::ModelStore& store;
    models::OvRuntime& runtime;
    const models::ModelInfo& info;
    nlohmann::json languages;
    std::int64_t eos = 2;
    std::int64_t decoder_start = 2;
    std::int64_t source_language = 0;
    std::mutex mutex;  // one translation at a time
    ov::Core core;     // tokenizer models need the tokenizers extension
    bool extension = false;
    ov::InferRequest tokenizer;
    ov::InferRequest detokenizer;
    ov::InferRequest encoder;
    ov::InferRequest decoder;
    std::atomic<bool> cancel{false};
    // Last, so its thread stops first
    std::unique_ptr<models::Residency> residency;

    // Called by Residency while no translation runs
    void Load() {
        try {
            LoadAndWarm();
        } catch (...) {
            Reset();
            throw;
        }
    }

    void LoadAndWarm() {
        const auto t0 = std::chrono::steady_clock::now();
        store.Verify(info);
        encoder = runtime.Load(info, "openvino_encoder_model.xml").model.create_infer_request();
        decoder = runtime.Load(info, "openvino_decoder_model.xml").model.create_infer_request();
        if (!extension) {
            core.add_extension("openvino_tokenizers.dll");
            extension = true;
        }
        tokenizer = core.compile_model((info.dir / "openvino_tokenizer.xml").string(), "CPU")
                        .create_infer_request();
        detokenizer = core.compile_model((info.dir / "openvino_detokenizer.xml").string(), "CPU")
                          .create_infer_request();
        // Warms CPU kernels before the first real sentence
        const auto& first = languages.at("languages").begin().value();
        TranslateSentences("Ready.", first.at("id").get<std::int64_t>());
        log::Printf("clinicavt-engine: translator warmed in %.1f s\n", SecondsSince(t0));
    }

    void Unload() {
        Reset();
        log::Printf("clinicavt-engine: translator released\n");
    }

    void Reset() {
        tokenizer = ov::InferRequest();
        detokenizer = ov::InferRequest();
        encoder = ov::InferRequest();
        decoder = ov::InferRequest();
    }

    // NLLB input: source language token, ids, end token
    std::vector<std::int64_t> Tokenize(const std::string& sentence) {
        ov::Tensor input(ov::element::string, ov::Shape{1});
        input.data<std::string>()[0] = PlainPunctuation(sentence);
        tokenizer.set_input_tensor(input);
        tokenizer.infer();
        const auto body = tokenizer.get_tensor("input_ids");
        const auto* first = body.data<const std::int64_t>();
        std::vector<std::int64_t> ids{source_language};
        ids.insert(ids.end(), first, first + body.get_size());
        ids.push_back(eos);
        return ids;
    }

    std::string Detokenize(const std::vector<std::int64_t>& ids) {
        ov::Tensor input(ov::element::i64, {1, ids.size()});
        std::copy(ids.begin(), ids.end(), input.data<std::int64_t>());
        detokenizer.set_input_tensor(input);
        detokenizer.infer();
        std::string text = detokenizer.get_output_tensor().data<std::string>()[0];
        // The detokenizer leaves the sentencepiece marker in
        const std::string marker = "\xE2\x96\x81";
        std::size_t at = 0;
        while ((at = text.find(marker, at)) != std::string::npos) {
            text.replace(at, marker.size(), " ");
        }
        while (!text.empty() && text.front() == ' ') {
            text.erase(text.begin());
        }
        return text;
    }

    // NLLB is sentence-level; translate line by line to keep structure and quality
    std::string TranslateLine(const std::string& line, std::int64_t target,
                              const std::function<void(const std::string&)>& partial = {}) {
        const auto source = Tokenize(line);
        ov::Tensor ids(ov::element::i64, {1, source.size()});
        std::copy(source.begin(), source.end(), ids.data<std::int64_t>());
        ov::Tensor mask(ov::element::i64, {1, source.size()});
        std::fill_n(mask.data<std::int64_t>(), source.size(), std::int64_t{1});

        encoder.set_tensor("input_ids", ids);
        encoder.set_tensor("attention_mask", mask);
        encoder.infer();
        const auto hidden = encoder.get_output_tensor(0);

        decoder.reset_state();
        ov::Tensor beam(ov::element::i32, {1});
        beam.data<std::int32_t>()[0] = 0;
        // First step feeds decoder start + forced target language; state holds the rest
        std::vector<std::int64_t> step = {decoder_start, target};
        std::vector<std::int64_t> generated;
        while (generated.size() < kMaxTokens && !cancel.load()) {
            ov::Tensor step_ids(ov::element::i64, {1, step.size()});
            std::copy(step.begin(), step.end(), step_ids.data<std::int64_t>());
            decoder.set_tensor("input_ids", step_ids);
            decoder.set_tensor("encoder_hidden_states", hidden);
            decoder.set_tensor("encoder_attention_mask", mask);
            decoder.set_tensor("beam_idx", beam);
            decoder.infer();

            const auto logits = decoder.get_tensor("logits");
            const auto& shape = logits.get_shape();  // [1, steps, vocab]
            const std::size_t vocab = shape[2];
            const float* last = logits.data<float>() + (shape[1] - 1) * vocab;
            auto token = static_cast<std::int64_t>(
                std::distance(last, std::max_element(last, last + vocab)));
            // Greedy decoding can loop on one token; the runner-up breaks the loop
            const auto size = generated.size();
            if (size >= 2 && token == generated[size - 1] && token == generated[size - 2]) {
                std::vector<float> copy(last, last + vocab);
                copy[static_cast<std::size_t>(token)] = -1e9f;
                token = static_cast<std::int64_t>(
                    std::distance(copy.begin(), std::max_element(copy.begin(), copy.end())));
            }
            if (token == eos) {
                break;
            }
            generated.push_back(token);
            step = {token};
            if (partial) {
                partial(Detokenize(generated));
            }
        }
        return Detokenize(generated);
    }

    // NLLB stops early on multi-sentence input, so translate one sentence at a time
    std::string TranslateSentences(const std::string& line, std::int64_t target,
                                   const std::function<void(const std::string&)>& partial = {}) {
        std::string out;
        std::size_t from = 0;
        while (from < line.size() && !cancel.load()) {
            std::size_t end = line.size();
            for (const char* mark : {". ", "? ", "! "}) {
                const auto at = line.find(mark, from);
                if (at != std::string::npos && at + 1 < end) {
                    end = at + 1;
                }
            }
            const std::string sentence = line.substr(from, end - from);
            if (sentence.find_first_not_of(" \t") != std::string::npos) {
                if (!out.empty()) {
                    out += ' ';
                }
                const std::string prefix = out;
                out += TranslateLine(sentence, target, [&](const std::string& text) {
                    if (partial) partial(prefix + text);
                });
            }
            from = end + (end < line.size() ? 1 : 0);
        }
        return out;
    }
};

NllbTranslator::NllbTranslator(const models::ModelStore& store, models::OvRuntime& runtime)
    : impl_(new Impl(store, runtime)) {
    impl_->languages = LoadLanguages(impl_->info.dir);
    const auto& special = impl_->languages.at("special");
    impl_->eos = special.at("eos").get<std::int64_t>();
    impl_->decoder_start = special.at("decoderStart").get<std::int64_t>();
    impl_->source_language = special.at("sourceLang").get<std::int64_t>();
    Impl* const impl = impl_.get();
    impl_->residency = std::make_unique<models::Residency>(
        [impl] { impl->Load(); }, [impl] { impl->Unload(); }, kIdleRelease);
}

NllbTranslator::~NllbTranslator() = default;

void NllbTranslator::Prepare() {
    impl_->residency->Want();
}

void NllbTranslator::Release() {
    impl_->residency->Release();
}

std::vector<std::string> NllbTranslator::Languages() {
    std::vector<std::string> names;
    for (const auto& [name, entry] : impl_->languages.at("languages").items()) {
        names.push_back(name);
    }
    return names;
}

std::string NllbTranslator::Translate(const std::string& sheet, const std::string& language,
                                      const Progress& progress) {
    // Sheets saved before line-ending normalisation may contain CR
    const std::string text = strings::UnixLines(sheet);
    if (text.empty()) {
        throw std::runtime_error("nothing to translate");
    }
    const auto& languages = impl_->languages.at("languages");
    if (!languages.contains(language)) {
        throw std::runtime_error("unknown language: " + language);
    }
    const auto target = languages.at(language).at("id").get<std::int64_t>();

    return impl_->residency->Use([&] {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cancel = false;
        const auto t0 = std::chrono::steady_clock::now();

        std::string translated;
        std::size_t from = 0;
        while (from <= text.size() && !impl_->cancel.load()) {
            const auto end = text.find('\n', from);
            const std::string line =
                text.substr(from, end == std::string::npos ? std::string::npos : end - from);
            if (!translated.empty()) {
                translated += '\n';
            }
            if (!line.empty() && line.find_first_not_of(" \t") != std::string::npos) {
                const std::string prefix = translated;
                translated += impl_->TranslateSentences(line, target, [&](const std::string& part) {
                    if (progress) progress(prefix + part);
                });
            }
            if (progress) {
                progress(translated);
            }
            if (end == std::string::npos) {
                break;
            }
            from = end + 1;
        }

        log::Printf("clinicavt-engine: translated to %s in %.1f s\n", language.c_str(),
                    SecondsSince(t0));
        return translated;
    });
}

void NllbTranslator::Cancel() {
    impl_->cancel = true;
}

}  // namespace clinicavt::translate
