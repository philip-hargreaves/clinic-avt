#pragma once

#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::ipc {

using nlohmann::json;

// JSON-RPC 2.0 reserved codes, plus the engine range -32000..-32099.
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kCaptureFailed = -32000;
inline constexpr int kSessionError = -32001;

inline constexpr int kProtocolVersion = 1;

// A string id echoes into every reply, so bounding it keeps replies encodable
inline constexpr std::size_t kMaxIdBytes = 128;

using Id = std::variant<std::int64_t, std::string>;

struct Request {
    Id id;
    std::string method;
    json params;
};

struct Error {
    int code = 0;
    std::string message;
    std::optional<json> data;
};

struct PeerInfo {
    std::string name;
    std::string version;
    int protocol_version = 0;
};

// Whisper output can carry invalid UTF-8, so replace rather than throw
inline std::string Serialize(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

inline json IdToJson(const Id& id) {
    if (std::holds_alternative<std::int64_t>(id)) return std::get<std::int64_t>(id);
    return std::get<std::string>(id);
}

// One request per frame, no batches: an integer or string id and object
// params. By value so params move out: nlohmann's copy recurses one frame
// per nesting level
inline std::variant<Request, Error> ParseRequest(json j) {
    const auto invalid = [](std::string why) {
        return Error{kInvalidRequest, "Invalid Request", json(std::move(why))};
    };
    if (!j.is_object()) return invalid("message is not an object");
    for (const auto& [key, value] : j.items()) {
        if (key != "jsonrpc" && key != "id" && key != "method" && key != "params") {
            return invalid("unknown member: " + key);
        }
    }
    if (!j.contains("jsonrpc") || j["jsonrpc"] != "2.0") return invalid("jsonrpc must be \"2.0\"");
    if (!j.contains("method") || !j["method"].is_string()) return invalid("method missing");
    if (!j.contains("id")) return invalid("notification received where request expected");
    if (j.contains("params") && !j["params"].is_object())
        return invalid("params must be an object");

    Request req;
    const auto& id = j["id"];
    if (id.is_number_integer()) {
        if (id.is_number_unsigned() &&
            id.get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return invalid("id out of range");
        }
        req.id = id.get<std::int64_t>();
    } else if (id.is_string()) {
        const auto& text = id.get_ref<const std::string&>();
        if (text.size() > kMaxIdBytes) return invalid("id string too long");
        req.id = text;
    } else {
        return invalid("id must be a string or integer");
    }
    req.method = j["method"].get<std::string>();
    req.params = j.contains("params") ? std::move(j["params"]) : json::object();
    return req;
}

inline json MakeResult(const Id& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", IdToJson(id)}, {"result", std::move(result)}};
}

inline json MakeError(const Id& id, const Error& e) {
    json err{{"code", e.code}, {"message", e.message}};
    if (e.data) err["data"] = *e.data;
    return json{{"jsonrpc", "2.0"}, {"id", IdToJson(id)}, {"error", std::move(err)}};
}

inline json MakeNotification(const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

inline json MakeRequest(const Id& id, const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"},
                {"id", IdToJson(id)},
                {"method", method},
                {"params", std::move(params)}};
}

inline json ToJson(const PeerInfo& p) {
    return json{{"name", p.name}, {"version", p.version}, {"protocolVersion", p.protocol_version}};
}

inline std::optional<PeerInfo> PeerInfoFromJson(const json& j) {
    if (!j.is_object() || j.size() != 3) return std::nullopt;
    if (!j.contains("name") || !j["name"].is_string()) return std::nullopt;
    if (!j.contains("version") || !j["version"].is_string()) return std::nullopt;
    // Compare on the json value: get<int>() would truncate an out-of-range number
    // into a match. There is only one supported version, so equality is the check.
    if (!j.contains("protocolVersion") || j["protocolVersion"] != kProtocolVersion) {
        return std::nullopt;
    }
    return PeerInfo{j["name"].get<std::string>(), j["version"].get<std::string>(),
                    kProtocolVersion};
}

// The two error shapes every handler returns
inline Error InvalidParams(std::string detail) {
    return Error{kInvalidParams, "Invalid params", json(std::move(detail))};
}

inline Error SessionError(std::string detail) {
    return Error{kSessionError, "Session error", json(std::move(detail))};
}

inline std::variant<std::string, Error> IdFrom(const json& params) {
    if (!params.contains("id") || !params["id"].is_string()) {
        return InvalidParams("id must be a string");
    }
    return params["id"].get<std::string>();
}

// A handler on one stored session: body(id) runs once the id is valid, and a
// store failure inside it becomes a session error
template <class Body>
std::variant<json, Error> WithSession(const json& params, Body body) {
    const auto id = IdFrom(params);
    if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
    try {
        return body(std::get<std::string>(id));
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

// Optional stamps are empty strings in the store and null on the wire
inline json NullWhenEmpty(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}

// A transcript turn as every message carries it
inline json TurnJson(const asr::Turn& turn) {
    return {{"firstFrame", turn.first_frame},
            {"frameCount", turn.frame_count},
            {"speaker", turn.speaker},
            {"text", turn.text}};
}

inline asr::Turn TurnFromJson(const json& t) {
    return {t.value("firstFrame", std::uint64_t{0}), t.value("frameCount", std::uint64_t{0}),
            t.value("speaker", ""), t.value("text", "")};
}

inline json TurnsJson(const std::vector<asr::Turn>& turns) {
    json list = json::array();
    for (const auto& turn : turns) list.push_back(TurnJson(turn));
    return list;
}

inline std::vector<asr::Turn> TurnsFromJson(const json& list) {
    std::vector<asr::Turn> turns;
    for (const auto& t : list) turns.push_back(TurnFromJson(t));
    return turns;
}

}  // namespace clinicavt::ipc
