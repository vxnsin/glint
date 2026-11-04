// glint: one binary, two roles. Without a command it is the daemon; with one it is a client
// that talks to the daemon over the socket (docs/IPC.md). M0 only knows --version and
// --help; the daemon loop arrives with M1.

#include <cstdio>
#include <cstring>
#include <string_view>

#include "core/log.hpp"
#include "core/version.hpp"
#include "ipc/protocol.hpp"

namespace {

void usage() {
    std::printf(
        "glint %s\n"
        "\n"
        "  glint                      run the daemon\n"
        "  glint outputs              list outputs and what they show\n"
        "  glint set <output|*> <path> [key=value ...]\n"
        "  glint unset <output|*>\n"
        "  glint status [output]\n"
        "  glint props <output> [name value ...]\n"
        "  glint pause|resume [output] [reason]\n"
        "  glint audio <output> volume=<0-1> [mute=on|off]\n"
        "  glint inspect <path>\n"
        "  glint --version | --help\n",
        glint::version);
}

int run_daemon() {
    glint::log::info("glint {} starting (M0 scaffold: no Wayland, no Vulkan yet)", glint::version);
    glint::log::info("nothing to do; the daemon loop comes with M1");
    return 0;
}

int run_client(std::string_view op) {
    bool known = false;
    for (auto candidate : glint::ipc::ops) {
        if (candidate == op) known = true;
    }
    if (!known) {
        std::fprintf(stderr, "glint: unknown command '%.*s'\n", static_cast<int>(op.size()),
                     op.data());
        usage();
        return 2;
    }
    std::fprintf(stderr, "glint: '%.*s' is not implemented yet, see docs/IPC.md\n",
                 static_cast<int>(op.size()), op.data());
    return 1;
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
        return run_client(argv[1]);
    }
    return run_daemon();
}
