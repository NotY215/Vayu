#pragma once
#include "Integrator.hpp"
#include "Vir.hpp"
#include <string>
#include <vector>

namespace vib {

    struct VirExecResult {
        bool        success = false;
        ErrorKind   kind = ErrorKind::RuntimeError;
        std::string message;
        int         exitCode = 0;
        std::vector<std::string> stdoutLines;
    };

    class VirInterpreter {
    public:
        VirExecResult execute(const VirModule& module) const;
    };

} // namespace vib