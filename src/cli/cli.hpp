#pragma once

// The client side of the binary: argv to a request on the socket, the reply to text or JSON.
namespace glint::cli {

int run(int argc, char** argv);
int run_menu();

}  // namespace glint::cli
