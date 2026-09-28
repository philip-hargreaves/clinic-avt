#include "adapters/models/ov_runtime.hpp"

#include <stdexcept>

namespace clinicavt::models {

ov::AnyMap CompileProperties(const ModelInfo& info) {
    ov::AnyMap map{{"CACHE_DIR", CacheDir(info).string()}};
    for (const auto& [key, value] : info.properties.items()) {
        if (value.is_boolean()) {
            map[key] = value.get<bool>();
        } else if (value.is_string()) {
            map[key] = value.get<std::string>();
        } else {
            map[key] = value.dump();
        }
    }
    return map;
}

std::string OvRuntime::ResolveDevice(const std::string& requested) {
    if (requested == "CPU") {
        return "CPU";
    }
    if (requested == "GPU" || requested == "NPU") {
        std::string listing;
        for (const auto& device : core_.get_available_devices()) {
            if (device.rfind(requested, 0) != 0) continue;
            const auto name = core_.get_property(device, ov::device::full_name);
            listing += " " + device + "=" + name;
            if (requested == "NPU" || name.find("Intel") != std::string::npos) {
                return device;
            }
        }
        throw std::runtime_error("no Intel " + requested +
                                 " available; found:" + (listing.empty() ? " nothing" : listing));
    }
    throw std::runtime_error("unsupported device in manifest: " + requested);
}

std::map<std::string, std::string> OvRuntime::DescribeDevices() {
    std::map<std::string, std::string> devices;
    for (const auto& device : core_.get_available_devices()) {
        try {
            std::string name = core_.get_property(device, ov::device::full_name);
            if (device.rfind("NPU", 0) == 0) {
                try {
                    name += " (arch " + core_.get_property(device, ov::device::architecture) + ")";
                } catch (...) {  // NOLINT(bugprone-empty-catch) named without the architecture
                }
            }
            devices[device] = name;
        } catch (...) {  // NOLINT(bugprone-empty-catch) device left out of the list
        }
    }
    return devices;
}

LoadedModel OvRuntime::Load(const ModelStore& store, std::string_view task, std::string_view tier,
                            const std::string& xml_name) {
    const ModelInfo& info = store.Resolve(task, tier);
    store.Verify(info);
    return Load(info, xml_name);
}

LoadedModel OvRuntime::Load(const ModelInfo& info, const std::string& xml_name) {
    const auto xml = info.dir / xml_name;
    if (!std::filesystem::exists(xml)) {
        throw std::runtime_error(info.id + ": no such model file " + xml_name);
    }

    LoadedModel loaded;
    loaded.device = ResolveDevice(info.device);
    // As with Whisper: first launch compiles and exports, later launches import the cached blob
    loaded.model = core_.compile_model(xml.string(), loaded.device, CompileProperties(info));
    return loaded;
}

}  // namespace clinicavt::models
