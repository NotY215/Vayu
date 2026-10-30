#pragma once
#include "Vir.hpp"
#include <string>
#include <vector>

namespace vib {

    struct VirExecResult {
        bool        success = false;
        ErrorKind   kind = ErrorKind::RuntimeError;
        std::string message;
        int         exitCode = 0;
        std::vector<std::string> stdoutLines;   // captured output, one entry per line
    };

    class VirInterpreter {
    public:
        // Executes `module` and captures stdout lines. Nothing is printed to
        // the real stdout by this class — the caller decides.
        VirExecResult execute(const VirModule& module) const;
    };

} // namespace vib