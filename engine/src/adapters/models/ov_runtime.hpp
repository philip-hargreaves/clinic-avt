#pragma once

#include <map>
#include <openvino/openvino.hpp>
#include <string>
#include <string_view>

#include "adapters/models/model_store.hpp"

namespace clinicavt::models {

// CACHE_DIR plus the manifest's properties. Values reach OpenVINO as the
// strings its own property parsing accepts ("32" for a float hint), bools
// as bools
ov::AnyMap CompileProperties(const ModelInfo& info);

struct LoadedModel {
    ov::CompiledModel model;
    std::string device;  // the concrete device compiled for, e.g. GPU.1
};

// Compiles cleared models for their manifest device. An unavailable device
// is a loud error naming what exists, with no fallback
class OvRuntime {
   public:
    LoadedModel Load(const ModelStore& store, std::string_view task, std::string_view tier,
                     const std::string& xml_name);
    // One IR of a model the caller has already resolved and verified
    LoadedModel Load(const ModelInfo& info, const std::string& xml_name);

    std::string ResolveDevice(const std::string& requested);

    // Device id -> driver-reported full name (NPU with its architecture)
    std::map<std::string, std::string> DescribeDevices();

   private:
    ov::Core core_;
};

}  // namespace clinicavt::models
