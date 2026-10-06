#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_security.hpp"

namespace clinicavt::ipc {

class PipeTaken : public std::runtime_error {
   public:
    PipeTaken() : std::runtime_error("pipe name already claimed by another process") {}
};

// One duplex pipe, one client at a time. The constructor claims the name and
// throws PipeTaken if it is already taken (treated as an attack)
class PipeServer {
   public:
    using MethodHandler = std::function<std::variant<json, Error>(const json& params)>;

    explicit PipeServer(const std::wstring& pipe_name);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    void RegisterMethod(const std::string& method, MethodHandler handler);

    // Handlers queue these, and each goes to the client right after the reply
    void QueueNotification(const std::string& method, json params);

    // Thread-safe. Bounded so a client that stops reading cannot stall capture
    void PushNotification(const std::string& method, json params);

    enum class Accept { kClient, kIdle };

    // Waits for a client. Returns once no client has come for `idle` while `busy`
    // is false. Clients that leave before sending are dropped and waiting continues
    Accept AwaitClient(std::chrono::milliseconds idle, const std::function<bool()>& busy = {});

    // Serves until disconnect or stream corruption. False if the client left
    // without sending a whole frame
    bool Serve();

    // Blocks while it accepts one client and serves until it disconnects or the stream corrupts
    void ServeOneClient();

   private:
    void HandleFrame(const std::string& payload);
    void Reply(const Id& id, const json& envelope);
    void FlushNotifications();
    bool WriteFrame(const std::string& payload, unsigned timeout_ms = 0);  // 0 waits forever

    PipeSecurity security_;
    void* pipe_ = nullptr;  // HANDLE
    std::map<std::string, MethodHandler> handlers_;
    std::vector<json> notifications_;
    std::mutex write_mutex_;
    bool write_failed_ = false;  // under write_mutex_, set when a torn frame ends the stream
};

}  // namespace clinicavt::ipc
