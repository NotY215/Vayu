// compiler/lint/Linter.hpp
#pragma once
#include "ast/Ast.hpp"
#include <string>
#include <vector>

namespace vayu::lint {

    struct Diagnostic {
        int         line = 1;
        int         col = 1;
        std::string message;
    };

    struct Options {
        bool unusedLocals = true;
        bool unreachableCode = true;
        bool emptyBodies = true;
        bool shadowing = false;   // off by default: noisy
    };

    struct Result {
        bool                     ok = false;
        std::string              error;
        std::vector<Diagnostic>  diagnostics;
    };

    Result check(const std::string& source, const Options& opts);
    int    run(int argc, char** argv);

}