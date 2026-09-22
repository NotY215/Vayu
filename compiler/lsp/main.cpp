// compiler/lsp/main.cpp
#include "LspServer.hpp"
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#endif

int main(int /*argc*/, char** /*argv*/) {
#ifdef _WIN32
    // LSP framing is byte-exact; disable CRT newline translation.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    vayu::lsp::LspServer server(std::cin, std::cout);
    return server.run();
}