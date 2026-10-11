#pragma once
#include "OpCode.hpp"
#include "interp/Value.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vayu {

    /// A compiled compilation unit -- top-level script or a single function body.
    struct Chunk {
        std::vector<uint8_t>     code;
        std::vector<int>         lines;
        std::vector<Value>       constants;
        std::vector<std::string> names;

        /// Nested functions declared inside this chunk .
        std::vector<std::shared_ptr<Chunk>> functions;

        /// Parameter names -- populated for function chunks; empty for top-level.
        std::vector<std::string> paramNames;

        void   emit(uint8_t b, int line);
        void   emitOp(OpCode op, int line);

        int    addConstant(Value v);
        int    addName(const std::string& name);
        int    addFunction(std::shared_ptr<Chunk> fn);

        size_t here() const { return code.size(); }

        void   patchJump(size_t pos, int target, int line);

        int    readU16(size_t at) const;
        int    readI16(size_t at) const;
    };

    void disassemble(const Chunk& chunk, const char* title);

} // namespace vayu