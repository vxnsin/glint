#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace glint::cli {

std::filesystem::path workshop_path();
std::filesystem::path download_path();
std::filesystem::path resolve_wallpaper(std::string_view target);
nlohmann::json list_library(const std::filesystem::path& workshop, std::string_view search,
                            std::string& error);
nlohmann::json list_installed_library(const std::filesystem::path& workshop,
                                      const std::filesystem::path& downloads,
                                      std::string_view search, std::string& error);
std::string register_workshop_download(const std::filesystem::path& folder,
                                       const std::filesystem::path& downloads);
nlohmann::json inspect_wallpaper(const std::filesystem::path& folder, std::string& error);

}  // namespace glint::cli
