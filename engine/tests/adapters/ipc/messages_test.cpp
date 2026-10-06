#include "adapters/ipc/messages.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace clinicavt::ipc {
namespace {

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(CLINICAVT_FIXTURE_DIR) + "/" + name);
    // Throwing here names the missing file. Parsing a closed stream would not
    if (!in.is_open()) {
        throw std::runtime_error("missing fixture: " + name);
    }
    return json::parse(in);
}

TEST(Messages, EnvelopesMatchTheFixtures) {
    // A request with an integer id, and the peer it describes
    const auto hello_parsed = ParseRequest(LoadFixture("hello-request.json"));
    ASSERT_TRUE(std::holds_alternative<Request>(hello_parsed));
    const auto& hello = std::get<Request>(hello_parsed);
    EXPECT_EQ(hello.method, "engine/hello");
    EXPECT_EQ(std::get<std::int64_t>(hello.id), 1);
    const auto peer = PeerInfoFromJson(hello.params);
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer->name, "clinicavt-shell");
    EXPECT_EQ(peer->version, "0.1.0");
    EXPECT_EQ(peer->protocol_version, 1);
    EXPECT_EQ(MakeResult(std::int64_t{1}, ToJson(PeerInfo{"clinicavt", "0.1.0", kProtocolVersion})),
              LoadFixture("hello-response.json"));

    // A string id and clinical non-ASCII text come back as they went in
    const auto echo_parsed = ParseRequest(LoadFixture("echo-request-nonascii.json"));
    ASSERT_TRUE(std::holds_alternative<Request>(echo_parsed));
    const auto& echo = std::get<Request>(echo_parsed);
    EXPECT_EQ(std::get<std::string>(echo.id), "e-2");
    EXPECT_EQ(MakeResult(echo.id, json{{"payload", echo.params["payload"]}}),
              LoadFixture("echo-response-nonascii.json"));

    EXPECT_EQ(MakeError(std::int64_t{7}, Error{kMethodNotFound, "Method not found"}),
              LoadFixture("error-method-not-found.json"));

    EXPECT_EQ(MakeNotification("audio.level", {{"level", 0.5}, {"clipped", false}}),
              LoadFixture("audio-level.json"));
    EXPECT_EQ(
        MakeNotification("session/interrupted",
                         {{"reason", "deviceLost"}, {"detail", "GetBuffer reported 0x88890004"}}),
        LoadFixture("session-interrupted.json"));
    EXPECT_EQ(MakeNotification("session/progress", {{"stage", "speakers"}}),
              LoadFixture("session-progress.json"));
}

TEST(Messages, ParseRequestRefusesWhatTheEnvelopeForbids) {
    const std::uint64_t beyond_int64 =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    struct Case {
        const char* what;
        json message;
    };
    const Case cases[] = {
        {"an unknown member",
         json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "m"}, {"extra", true}}},
        {"a null id", json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "m"}}},
        {"no jsonrpc member", json{{"id", 1}, {"method", "m"}}},
        {"a batch", json::array({json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "m"}}})},
        // The id is echoed into the reply, so its size is bounded
        {"an overlong string id",
         json{{"jsonrpc", "2.0"}, {"id", std::string(kMaxIdBytes + 1, 'x')}, {"method", "m"}}},
        {"an id beyond int64", json{{"jsonrpc", "2.0"}, {"id", beyond_int64}, {"method", "m"}}},
    };
    for (const auto& c : cases) {
        const auto parsed = ParseRequest(c.message);
        ASSERT_TRUE(std::holds_alternative<Error>(parsed)) << c.what;
        EXPECT_EQ(std::get<Error>(parsed).code, kInvalidRequest) << c.what;
    }
}

// Whisper can emit invalid UTF-8. A throw here would drop the reply
TEST(Messages, SerializeReplacesInvalidUtf8AndKeepsValidText) {
    std::string out;
    EXPECT_NO_THROW(out = Serialize(json{{"payload", std::string("bad \xFF\xFE bytes")}}));
    EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);  // U+FFFD

    EXPECT_NE(Serialize(json{{"payload", "naïve café 東京 µg °C"}}).find("naïve café 東京"),
              std::string::npos);
}

TEST(Messages, DeeplyNestedParamsDoesNotOverflow) {
    // nlohmann parse and destruction are iterative. Only a copy recurses, so
    // ParseRequest must move params out. The depth is far past what a 1 MB stack holds
    constexpr int kDepth = 200000;
    std::string nested;
    nested.reserve(kDepth * 6 + 32);
    for (int i = 0; i < kDepth; ++i) nested += "{\"a\":";
    nested += "1";
    for (int i = 0; i < kDepth; ++i) nested += "}";
    json message{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "m"}, {"params", json::object()}};
    message["params"]["deep"] = json::parse(nested);

    auto parsed = ParseRequest(std::move(message));
    EXPECT_TRUE(std::holds_alternative<Request>(parsed));
}

}  // namespace
}  // namespace clinicavt::ipc
