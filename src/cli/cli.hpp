#pragma once

#include <nlohmann/json.hpp>
#include <stop_token>

// The client side of the binary: argv to a request on the socket, the reply to text or JSON.
namespace glint::cli {

int run(int argc, char** argv);
nlohmann::json request_daemon(const nlohmann::json& request, std::stop_token stop = {});

}  // namespace glint::cli
