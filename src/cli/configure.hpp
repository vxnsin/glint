#pragma once

#include <functional>
#include <iosfwd>
#include <nlohmann/json.hpp>
#include <string>

namespace glint::cli {

// The callback sends one request and returns the daemon's full reply.
using Exchange = std::function<nlohmann::json(const nlohmann::json&)>;
int configure(const std::string& output, std::istream& in, std::ostream& out,
              const Exchange& exchange);
int choose_wallpaper(const nlohmann::json& library, std::istream& in, std::ostream& out,
                     const Exchange& exchange);

}  // namespace glint::cli
