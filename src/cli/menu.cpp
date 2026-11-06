#include "cli/menu.hpp"

#include <istream>
#include <ostream>

namespace glint::cli {

int menu(std::istream& in, std::ostream& out, const MenuCommand& command) {
    std::string input;
    for (;;) {
        out << "\nglint\n"
               "  1. Choose a wallpaper\n"
               "  2. Wallpaper settings\n"
               "  3. List displays\n"
               "  4. Show status\n"
               "  5. Pause wallpapers\n"
               "  6. Resume wallpapers\n"
               "  q. Quit\n"
               "Select an option: "
            << std::flush;
        if (!std::getline(in, input) || input == "q" || input == "Q") return 0;
        if (input == "1") {
            out << "Search installed wallpapers (Enter for all): " << std::flush;
            if (!std::getline(in, input)) return 0;
            if (input.empty())
                command({"choose"});
            else
                command({"choose", input});
        } else if (input == "2") {
            if (command({"outputs"}) != 0) continue;
            out << "Display name (Enter to cancel): " << std::flush;
            if (!std::getline(in, input)) return 0;
            if (!input.empty()) command({"configure", input});
        } else if (input == "3") {
            command({"outputs"});
        } else if (input == "4") {
            command({"status"});
        } else if (input == "5") {
            command({"pause", "*", "request"});
        } else if (input == "6") {
            command({"resume", "*", "request"});
        } else {
            out << "Select an option from the list.\n";
        }
    }
}

}  // namespace glint::cli
