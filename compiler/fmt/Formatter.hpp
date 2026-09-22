// compiler/fmt/Formatter.hpp
#pragma once
#include <string>

namespace vayu::fmt {

    struct Options {
        bool inPlace = false;
        bool checkOnly = false;
        int  indentWidth = 4;
    };

    struct Result {
        bool        ok = false;
        int         exitCode = 0;
        std::string error;
        std::string formatted;
    };

    Result process(const std::string& source, const Options& opts);
    void   printUsage();
    int    run(int argc, char** argv);

}