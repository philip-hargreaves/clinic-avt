#pragma once

#include <memory>
#include <string>
#include <vector>

#include "adapters/interfaces/translator.hpp"

namespace clinicavt::models {
class ModelStore;
class OvRuntime;
}  // namespace clinicavt::models

namespace clinicavt::translate {

// NLLB-200 on CPU. Runs the encoder once, then a greedy stateful decode. Loads on Prepare or first
// use and unloads on Release or after 10 idle minutes
class NllbTranslator : public ITranslator {
   public:
    NllbTranslator(const models::ModelStore& store, models::OvRuntime& runtime);
    ~NllbTranslator() override;

    std::vector<std::string> Languages() override;

    void Prepare() override;

    void Release() override;

    std::string Translate(const std::string& text, const std::string& language,
                          const Progress& progress) override;

    void Cancel() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::translate
