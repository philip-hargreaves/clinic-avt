#pragma once

#include <map>
#include <openvino/openvino.hpp>
#include <string>
#include <string_view>

#include "adapters/models/model_store.hpp"

namespace clinicavt::models {

// CACHE_DIR plus the manifest properties. Values are passed as the strings OpenVINO parses
// ("32" for a float hint), and bools stay bools
ov::AnyMap CompileProperties(const ModelInfo& info);

struct LoadedModel {
    ov::CompiledModel model;
    std::string device;  // the concrete device compiled for, e.g. GPU.1
};

// Compiles models for their manifest device. A missing device throws, listing the available
// devices, with no fallback
class OvRuntime {
   public:
    LoadedModel Load(const ModelStore& store, std::string_view task, std::string_view tier,
                     const std::string& xml_name);
    // The caller has already resolved and verified the model
    LoadedModel Load(const ModelInfo& info, const std::string& xml_name);

    std::string ResolveDevice(const std::string& requested);

    // Device id -> driver-reported full name (NPU with its architecture)
    std::map<std::string, std::string> DescribeDevices();

   private:
    ov::Core core_;
};

}  // namespace clinicavt::models
