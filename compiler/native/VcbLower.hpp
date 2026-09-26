#pragma once
#include "ast/Ast.hpp"
#include <string>

namespace vayu {

    // Lower a Vayu AST to VCB IR text (the format vcb.exe consumes).
    // Throws std::runtime_error on any construct not yet supported.
    class VcbLower {
    public:
        std::string lower(const Block& program, const std::string& sourceDir);
    };

} // namespace vayu