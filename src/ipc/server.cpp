#include "ipc/server.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "core/log.hpp"
#include "core/loop.hpp"

namespace glint::ipc {

namespace {

// A client that sends this much without a newline is dropped.
constexpr std::size_t max_line = 1 << 20;
constexpr std::size_t max_outgoing = 4u << 20;  // bytes queued for one client

bool fill_address(sockaddr_un& address, const std::string& path) {
    address = {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

void Server::Reply::ok(nlohmann::json result) {
    if (server_ == nullptr || !has_id_) return;
    Server* server = std::exchange(server_, nullptr);
    if (Client* client = server->find(client_.lock())) {
        server->send(*client, {{"id", id_}, {"ok", true}, {"result", std::move(result)}});
    }
}

void Server::Reply::error(std::string message) {
    if (server_ == nullptr || !has_id_) return;
    Server* server = std::exchange(server_, nullptr);
    if (Client* client = server->find(client_.lock())) {
        server->send(*client, {{"id", id_}, {"ok", false}, {"error", std::move(message)}});
    }
}

Server::Server(Loop& loop, Handler handler) : loop_(loop), handler_(std::move(handler)) {}

Server::~Server() {
    for (auto& client : clients_) loop_.remove(client->loop_id);
    if (listen_fd_) {
        loop_.remove(listen_id_);
        ::unlink(path_.c_str());
    }
}

std::string Server::listen(const std::string& path) {
    sockaddr_un address;
    if (!fill_address(address, path)) return "socket path too long: " + path;
    std::error_code ec;
    auto dir = std::filesystem::path(path).parent_path();
    bool made = !std::filesystem::exists(dir, ec) && std::filesystem::create_directories(dir, ec);
    // Only a directory of glint's own is made private; GLINT_SOCKET may point anywhere.
    if (made || dir.filename() == "glint") ::chmod(dir.c_str(), 0700);

    listen_fd_.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!listen_fd_) return std::string("socket: ") + std::strerror(errno);
    if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EADDRINUSE) return "bind " + path + ": " + std::strerror(errno);
        // A socket file is there: if a daemon answers on it, it is in use; else it was left
        // behind, and the bind is tried once more without it.
        UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (::connect(probe.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return "another glint is already running on " + path;
        }
        ::unlink(path.c_str());
        if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return "bind " + path + ": " + std::strerror(errno);
        }
    }
    ::chmod(path.c_str(), 0600);
    if (::listen(listen_fd_.get(), 16) != 0) return std::string("listen: ") + std::strerror(errno);
    path_ = path;
    listen_id_ = loop_.add(listen_fd_.get(), POLLIN, [this](short) { accept(); });
    log::info("listening on {}", path);
    return {};
}

void Server::accept() {
    for (;;) {
        int fd = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        auto client = std::make_unique<Client>();
        client->token = std::make_shared<int>(fd);
        client->fd.reset(fd);
        Client* raw = client.get();
        client->loop_id = loop_.add(fd, POLLIN, [this, raw](short revents) {
            if ((revents & POLLOUT) != 0) writable(*raw);
            if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) readable(*raw);
        });
        clients_.push_back(std::move(client));
    }
}

Server::Client* Server::find(const std::shared_ptr<int>& token) {
    if (!token) return nullptr;
    for (auto& client : clients_) {
        if (client->token == token) return client.get();
    }
    return nullptr;
}

void Server::drop(Client& client) {
    loop_.remove(client.loop_id);
    std::erase_if(clients_, [&](const auto& c) { return c.get() == &client; });
}

void Server::readable(Client& client) {
    char buffer[4096];
    for (;;) {
        ssize_t n = ::read(client.fd.get(), buffer, sizeof(buffer));
        if (n > 0) {
            client.in.append(buffer, static_cast<std::size_t>(n));
            if (client.in.size() > max_line) break;  // a flood: dropped below
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        drop(client);  // closed or failed
        return;
    }
    std::shared_ptr<int> token = client.token;
    std::size_t start = 0;
    for (;;) {
        std::size_t end = client.in.find('\n', start);
        if (end == std::string::npos) break;
        std::string line = client.in.substr(start, end - start);
        start = end + 1;
        if (line.empty() || line == "\r") continue;

        Reply reply;
        reply.server_ = this;
        reply.client_ = token;
        nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object()) {
            reply.has_id_ = true;  // unparsable: answer with a null id
            reply.error("not a JSON object");
            continue;
        }
        if (request.contains("id")) {
            reply.id_ = request["id"];
            reply.has_id_ = true;
        }
        std::string op = request.contains("op") && request["op"].is_string()
                             ? request["op"].get<std::string>()
                             : "";
        if (op == "subscribe") {
            client.events.clear();
            if (request.contains("events") && request["events"].is_array()) {
                for (const auto& name : request["events"]) {
                    if (name.is_string()) client.events.insert(name.get<std::string>());
                }
            } else {
                client.events = {"state", "props", "outputs", "error", "settings"};
            }
            reply.ok(nlohmann::json(
                std::vector<std::string>(client.events.begin(), client.events.end())));
            continue;
        }
        try {
            handler_(request, std::move(reply));
        } catch (const std::exception& e) {
            // A field of the wrong type, or anything else: the request fails, not glint.
            Reply failed;
            failed.server_ = this;
            failed.client_ = token;
            failed.has_id_ = true;
            if (request.contains("id")) failed.id_ = request["id"];
            failed.error(std::string("bad request: ") + e.what());
        }
        // The handler may have dropped this client (it cannot, today) or added others.
        if (find(token) == nullptr) return;
    }
    client.in.erase(0, start);
    if (client.in.size() > max_line) drop(client);
}

void Server::send(Client& client, const nlohmann::json& message) {
    bool was_empty = client.out.empty();
    client.out += message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    client.out += '\n';
    if (client.out.size() > max_outgoing) {
        // A subscriber that never reads: the daemon does not keep its backlog.
        client.out.clear();
        drop(client);
        return;
    }
    if (was_empty) writable(client);
}

void Server::writable(Client& client) {
    while (!client.out.empty()) {
        ssize_t n = ::write(client.fd.get(), client.out.data(), client.out.size());
        if (n > 0) {
            client.out.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EAGAIN) break;
        if (n < 0 && errno == EINTR) continue;
        client.out.clear();  // gone; readable() notices
        break;
    }
    loop_.modify(client.loop_id, client.out.empty() ? POLLIN : POLLIN | POLLOUT);
}

void Server::broadcast(const std::string& event, nlohmann::json body) {
    body["event"] = event;
    for (auto& client : clients_) {
        if (client->events.contains(event)) send(*client, body);
    }
}

}  // namespace glint::ipc
