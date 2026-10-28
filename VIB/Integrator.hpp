#pragma once
#include "compiler/ast/Ast.hpp"
#include <string>
namespace vib {
struct CompileResult { bool success = false; std::string message; std::string artifactPath; };
class Integrator {
public:
    CompileResult compile(const vayu::Block& program, const std::string& sourceDir,
                          const std::string& outputPath) const;
};
} // namespace vib
