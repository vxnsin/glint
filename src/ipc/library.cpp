#include "ipc/library.hpp"

#include "cli/library.hpp"

namespace glint::ipc {

std::optional<LibraryRequest> parse_library_request(const nlohmann::json& request,
                                                    std::string& error) {
    error.clear();
    if (!request.is_object() || !request.contains("op") || !request["op"].is_string()) {
        error = "library operation is required";
        return std::nullopt;
    }
    const auto op = request["op"].get<std::string>();
    if (op == "list") {
        if (request.contains("search") && !request["search"].is_string()) {
            error = "search must be a string";
            return std::nullopt;
        }
        return LibraryRequest{LibraryRequest::Operation::list, cli::workshop_path(),
                              request.value("search", std::string{})};
    }
    if (op == "inspect") {
        if (!request.contains("path") || !request["path"].is_string() ||
            request["path"].get<std::string>().empty()) {
            error = "inspect needs a path or Workshop id";
            return std::nullopt;
        }
        return LibraryRequest{LibraryRequest::Operation::inspect,
                              cli::resolve_wallpaper(request["path"].get<std::string>()),
                              {}};
    }
    error = "unknown library operation";
    return std::nullopt;
}

nlohmann::json execute_library_request(const LibraryRequest& request, std::string& error) {
    if (request.operation == LibraryRequest::Operation::list)
        return cli::list_library(request.path, request.search, error);
    return cli::inspect_wallpaper(request.path, error);
}

}  // namespace glint::ipc
