#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace glint::ipc {

struct LibraryRequest {
    enum class Operation { list, inspect } operation;
    std::filesystem::path path;
    std::string search;
    std::filesystem::path downloads;
};

std::optional<LibraryRequest> parse_library_request(const nlohmann::json& request,
                                                    std::string& error);
nlohmann::json execute_library_request(const LibraryRequest& request, std::string& error);

}  // namespace glint::ipc
