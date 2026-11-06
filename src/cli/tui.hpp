#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace glint::cli {

int run_tui();
nlohmann::json tui_setting_request(const std::string& output, const nlohmann::json& status,
                                   const std::string& name, const nlohmann::json& value);
std::string workshop_item_id(const std::string& text);
std::string kitty_image(const std::string& rgb, int width, int height, int columns, int rows,
                        unsigned id);

}  // namespace glint::cli
