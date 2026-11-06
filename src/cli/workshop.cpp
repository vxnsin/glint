#include "cli/workshop.hpp"

#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "cli/library.hpp"
#include "cli/tui.hpp"

extern char** environ;

namespace glint::cli {
std::vector<std::string> steamcmd_download_arguments(const std::string& id,
                                                     const std::string& account,
                                                     const std::filesystem::path& install_dir) {
    if (workshop_item_id(id) != id || id.empty())
        throw std::invalid_argument("Invalid Workshop item ID");
    if (account.empty() || account.size() > 64 ||
        !std::ranges::all_of(account, [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '.' || c == '-';
        }))
        throw std::invalid_argument("Enter your Steam account name, without a password");
    return {"steamcmd",
            "+force_install_dir",
            install_dir.string(),
            "+login",
            account,
            "+workshop_download_item",
            "431960",
            id,
            "+quit"};
}

std::string download_workshop(const std::string& id, const std::string& account) {
    const auto install_dir = download_path().parent_path().parent_path() / "steamcmd";
    std::vector<std::string> args;
    try {
        args = steamcmd_download_arguments(id, account, install_dir);
    } catch (const std::exception& e) {
        return e.what();
    }
    std::error_code ec;
    std::filesystem::create_directories(install_dir, ec);
    if (ec) return "Cannot create SteamCMD download directory: " + ec.message();
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid;
    const int error = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error != 0)
        return error == ENOENT
                   ? "SteamCMD is not installed. Install steamcmd, or subscribe through Steam."
                   : std::strerror(error);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return "Could not wait for SteamCMD";
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return "SteamCMD did not finish successfully. Check its output above.";
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = home ? home : "/";
    const std::vector<std::filesystem::path> roots = {
        install_dir,
        install_dir.parent_path(),
        workshop_path().parent_path().parent_path().parent_path().parent_path(),
        base / ".local/share/Steam",
        base / ".local/share/SteamCmd",
        base / ".local/share/steamcmd",
        base / ".steam/steamcmd"};
    std::string last_error;
    for (const auto& root : roots) {
        const auto folder = root / "steamapps/workshop/content/431960" / id;
        if (!std::filesystem::exists(folder / "project.json", ec)) continue;
        auto problem = register_workshop_download(folder, download_path());
        if (problem.empty()) return {};
        last_error = std::move(problem);
    }
    return last_error.empty() ? "No complete downloaded project was found. Use Import downloaded "
                                "folder with the path printed by SteamCMD."
                              : last_error;
}
}  // namespace glint::cli
