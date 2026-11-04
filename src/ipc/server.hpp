#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "core/fd.hpp"

namespace glint {
class Loop;
}

namespace glint::ipc {

// The daemon's socket: JSON lines in both directions (docs/IPC.md). Requests go to the
// handler; it answers through the Reply, now or later. Events go to subscribed clients.
class Server {
public:
    // Sends {"id": ..., "ok": ..., "result"|"error": ...}; a no-op if the request had no id or
    // the client is gone. May be called once.
    class Reply {
    public:
        void ok(nlohmann::json result = nullptr);
        void error(std::string message);

    private:
        friend class Server;
        Server* server_ = nullptr;
        std::weak_ptr<int> client_;
        nlohmann::json id_;
        bool has_id_ = false;
    };
    using Handler = std::function<void(const nlohmann::json& request, Reply reply)>;

    Server(Loop& loop, Handler handler);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds the socket. Fails if another daemon answers on it.
    std::string listen(const std::string& path);

    // To every client subscribed to the event name.
    void broadcast(const std::string& event, nlohmann::json body);

private:
    struct Client {
        std::shared_ptr<int> token;  // identity for replies that outlive the client
        UniqueFd fd;
        int loop_id = 0;
        std::string in;
        std::string out;
        std::set<std::string> events;
    };

    void accept();
    void readable(Client& client);
    void writable(Client& client);
    void send(Client& client, const nlohmann::json& message);
    void drop(Client& client);
    Client* find(const std::shared_ptr<int>& token);

    Loop& loop_;
    Handler handler_;
    UniqueFd listen_fd_;
    int listen_id_ = 0;
    std::string path_;
    std::vector<std::unique_ptr<Client>> clients_;
};

}  // namespace glint::ipc
