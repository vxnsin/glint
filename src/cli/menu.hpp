#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace glint::cli {

using MenuCommand = std::function<int(const std::vector<std::string>&)>;
int menu(std::istream& in, std::ostream& out, const MenuCommand& command);

}  // namespace glint::cli
