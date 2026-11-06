// glint: one binary, two roles. Without a command it is the daemon; with one it is a client
// that talks to the daemon over the socket (docs/IPC.md).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "core/log.hpp"
#include "core/version.hpp"
#include "ipc/protocol.hpp"

#ifdef GLINT_PLATFORM
#include "cli/cli.hpp"
#include "daemon.hpp"
#endif

namespace {

void usage() {
    std::printf(
        "glint %s\n"
        "\n"
        "  glint                      run the daemon\n"
        "  glint outputs              list outputs and what they show\n"
        "  glint list [search]        search installed Workshop wallpapers\n"
        "  glint choose [search]      choose an installed wallpaper and output\n"
        "  glint inspect <id|folder>  inspect a wallpaper without a daemon\n"
        "  glint set <output|*> <path> [key=value ...]\n"
        "  glint unset <output|*>\n"
        "  glint status [output]\n"
        "  glint pause|resume [output|*] [locked|request]\n"
        "  glint props <output> [reset | name value ...]\n"
        "  glint configure <output>   interactively edit wallpaper settings\n"
        "  glint screenshot <output> <file.png>\n"
        "  glint subscribe [state|props|outputs|error ...]\n"
        "  glint version\n"
        "  glint --version | --help\n"
        "\n"
        "  set options: scaling=fill|fit|center|tile|stretch color=#rrggbb\n"
        "               transition=fade|none duration=<ms>\n"
        "  add --json to any command for the raw reply.\n"
        "  set takes a picture, a scene folder or a Workshop id.\n"
        "  GLINT_WORKSHOP overrides the Workshop folder for list, inspect and set.\n",
        glint::version);
}

int run_daemon() {
    const char* level = std::getenv("GLINT_LOG");
    if (level != nullptr && std::strcmp(level, "debug") == 0) {
        glint::log::set_level(glint::log::Level::debug);
    }
#ifdef GLINT_PLATFORM
    glint::log::info("glint {} starting", glint::version);
    glint::Daemon daemon;
    return daemon.run();
#else
    glint::log::error("built without GLINT_PLATFORM, there is no daemon in this binary");
    return 1;
#endif
}

int run_client(int argc, char** argv) {
#ifdef GLINT_PLATFORM
    return glint::cli::run(argc, argv);
#else
    (void)argc;
    std::string_view op = argv[1];
    bool known = false;
    for (auto candidate : glint::ipc::ops) {
        if (candidate == op) known = true;
    }
    std::fprintf(stderr, "glint: %s '%.*s' in a build without GLINT_PLATFORM\n",
                 known ? "no client for" : "unknown command", static_cast<int>(op.size()),
                 op.data());
    return known ? 1 : 2;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0) {
            std::printf("glint %s (protocol %d)\n", glint::version, glint::protocol_version);
            return 0;
        }
        if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
            usage();
            return 0;
        }
        return run_client(argc, argv);
    }
    return run_daemon();
}
