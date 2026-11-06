#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace glint::cli {

std::vector<std::string> steamcmd_download_arguments(const std::string& id,
                                                     const std::string& account,
                                                     const std::filesystem::path& install_dir);
std::string download_workshop(const std::string& id, const std::string& account);

}  // namespace glint::cli
