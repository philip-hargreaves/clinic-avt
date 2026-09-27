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

// Another process already serves the pipe name
class PipeTaken : public std::runtime_error {
   public:
    PipeTaken() : std::runtime_error("pipe name already claimed by another process") {}
};

// One duplex pipe, one client at a time. Construction claims the pipe
// name, so a name already taken is treated as an attack and throws PipeTaken.
class PipeServer {
   public:
    using MethodHandler = std::function<std::variant<json, Error>(const json& params)>;

    explicit PipeServer(const std::wstring& pipe_name);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    void RegisterMethod(const std::string& method, MethodHandler handler);

    // Handlers may queue these. Each is written to the client right after the reply
    void QueueNotification(const std::string& method, json params);

    // Callable from any thread. Bounded so a client that stops draining never
    // stalls the capture thread
    void PushNotification(const std::string& method, json params);

    enum class Accept { kClient, kIdle };

    // Waits for the next client. Gives up once nobody has come for `idle`
    // while `busy` was false. A client that leaves before speaking is
    // dropped and the wait goes on
    Accept AwaitClient(std::chrono::milliseconds idle, const std::function<bool()>& busy = {});

    // Serves the connected client until it disconnects or the stream corrupts.
    // False when it left without sending a whole frame
    bool Serve();

    // Blocks: accept one client, serve until it disconnects or the stream corrupts
    void ServeOneClient();

   private:
    void HandleFrame(const std::string& payload);
    void Reply(const Id& id, const json& envelope);
    void FlushNotifications();
    bool WriteFrame(const std::string& payload, unsigned timeout_ms = 0);  // 0: wait forever

    PipeSecurity security_;
    void* pipe_ = nullptr;  // HANDLE
    std::map<std::string, MethodHandler> handlers_;
    std::vector<json> notifications_;
    std::mutex write_mutex_;
    bool write_failed_ = false;  // under write_mutex_. A torn frame ends the stream
};

}  // namespace clinicavt::ipc
