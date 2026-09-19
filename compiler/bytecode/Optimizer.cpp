#include "Optimizer.hpp"
#include <cmath>
#include <unordered_map>
#include <vector>

namespace vayu {

    // ---------------------------------------------------------------------------
    // Instruction length at a given offset.
    // ---------------------------------------------------------------------------
    static size_t insLen(const std::vector<uint8_t>& code, size_t pc) {
        if (pc >= code.size()) return 0;
        switch (static_cast<OpCode>(code[pc])) {
        case OpCode::CONST:
        case OpCode::LOAD:
        case OpCode::STORE:
        case OpCode::DEFINE:
        case OpCode::LIST_NEW:
        case OpCode::MAP_NEW:
        case OpCode::MAKE_FN:
        case OpCode::MAKE_GENERATOR:
        case OpCode::ATTR_GET:
        case OpCode::ATTR_SET:
        case OpCode::IMPORT:
        case OpCode::IMPORT_MEMBER:
        case OpCode::EXCEPT_MATCH:
        case OpCode::JUMP:
        case OpCode::JUMP_IF_FALSE:
        case OpCode::JUMP_IF_TRUE:
        case OpCode::ITER_NEXT:
        case OpCode::TRY_BEGIN:
        case OpCode::ADDR_OF:
        case OpCode::ADDR_OF_ATTR:
            return 3;

        case OpCode::NEW_INSTANCE:
            return 4;

        case OpCode::CALL:
            return 2;

        default:
            return 1;
        }
    }

    // ---------------------------------------------------------------------------
    // Constant folding
    // ---------------------------------------------------------------------------
    static bool foldBinaryOp(Value a, Value b, OpCode op, Value& out) {
        switch (op) {
        case OpCode::ADD:
            if (a.isInt() && b.isInt()) { out = Value(a.asInt() + b.asInt());       return true; }
            if (a.isNumber() && b.isNumber()) { out = Value(a.asDouble() + b.asDouble()); return true; }
            if (a.isString() && b.isString()) { out = Value(a.asString() + b.asString()); return true; }
            return false;
        case OpCode::SUB:
            if (a.isInt() && b.isInt()) { out = Value(a.asInt() - b.asInt());       return true; }
            if (a.isNumber() && b.isNumber()) { out = Value(a.asDouble() - b.asDouble()); return true; }
            return false;
        case OpCode::MUL:
            if (a.isInt() && b.isInt()) { out = Value(a.asInt() * b.asInt());       return true; }
            if (a.isNumber() && b.isNumber()) { out = Value(a.asDouble() * b.asDouble()); return true; }
            return false;
        case OpCode::DIV:
            if (a.isNumber() && b.isNumber() && b.asDouble() != 0.0)
            {
                out = Value(a.asDouble() / b.asDouble()); return true;
            }
            return false;
        case OpCode::FLOORDIV:
            if (a.isNumber() && b.isNumber() && b.asDouble() != 0.0) {
                double q = std::floor(a.asDouble() / b.asDouble());
                out = (a.isInt() && b.isInt()) ? Value((long long)q) : Value(q);
                return true;
            }
            return false;
        case OpCode::MOD:
            if (a.isNumber() && b.isNumber() && b.asDouble() != 0.0) {
                double m = std::fmod(a.asDouble(), b.asDouble());
                if (m != 0 && ((m < 0) != (b.asDouble() < 0))) m += b.asDouble();
                out = (a.isInt() && b.isInt()) ? Value((long long)m) : Value(m);
                return true;
            }
            return false;
        case OpCode::POW:
            if (a.isInt() && b.isInt() && b.asInt() >= 0) {
                long long base = a.asInt(), exp = b.asInt(), acc = 1;
                while (exp--) acc *= base;
                out = Value(acc); return true;
            }
            if (a.isNumber() && b.isNumber())
            {
                out = Value(std::pow(a.asDouble(), b.asDouble())); return true;
            }
            return false;
        case OpCode::EQ:
            out = Value(a.isNumber() && b.isNumber()
                ? a.asDouble() == b.asDouble()
                : (a.isString() && b.isString()
                    ? a.asString() == b.asString()
                    : (a.isBool() && b.isBool()
                        ? a.asBool() == b.asBool()
                        : a.isNone() && b.isNone())));
            return true;
        case OpCode::NEQ: {
            bool eq = a.isNumber() && b.isNumber()
                ? a.asDouble() == b.asDouble()
                : (a.isString() && b.isString()
                    ? a.asString() == b.asString()
                    : (a.isBool() && b.isBool()
                        ? a.asBool() == b.asBool()
                        : a.isNone() && b.isNone()));
            out = Value(!eq);
            return true;
        }
        case OpCode::LT: case OpCode::GT:
        case OpCode::LE: case OpCode::GE: {
            bool res = false;
            if (a.isNumber() && b.isNumber()) {
                double x = a.asDouble(), y = b.asDouble();
                switch (op) {
                case OpCode::LT: res = x < y; break;
                case OpCode::GT: res = x > y; break;
                case OpCode::LE: res = x <= y; break;
                case OpCode::GE: res = x >= y; break;
                default: break;
                }
            }
            else if (a.isString() && b.isString()) {
                const auto& x = a.asString(); const auto& y = b.asString();
                switch (op) {
                case OpCode::LT: res = x < y; break;
                case OpCode::GT: res = x > y; break;
                case OpCode::LE: res = x <= y; break;
                case OpCode::GE: res = x >= y; break;
                default: break;
                }
            }
            else return false;
            out = Value(res);
            return true;
        }
        default:
            return false;
        }
    }

    static bool foldUnaryOp(Value a, OpCode op, Value& out) {
        if (op == OpCode::NEG) {
            if (a.isInt()) { out = Value(-a.asInt());   return true; }
            if (a.isFloat()) { out = Value(-a.asFloat()); return true; }
            return false;
        }
        if (op == OpCode::NOT) {
            out = Value(a.isBool() ? !a.asBool() : !a.truthy());
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Main pass
    // ---------------------------------------------------------------------------
    void optimizeChunk(Chunk& chunk, OptStats& stats) {
        for (auto& fn : chunk.functions)
            if (fn) optimizeChunk(*fn, stats);

        if (chunk.code.empty()) return;

        const std::vector<uint8_t> oldCode = chunk.code;
        const std::vector<int>     oldLines = chunk.lines;

        std::vector<uint8_t> newCode;
        std::vector<int>     newLines;

        // srcStart -> dstStart
        std::unordered_map<size_t, size_t> map;
        // srcStart -> srcNext (may differ from srcStart + insLen when folded)
        std::unordered_map<size_t, size_t> nextSrc;

        // Helper: emit 3 bytes with 3 identical lines (constants / LOAD-like ops).
        auto emit3 = [&](uint8_t b0, uint8_t b1, uint8_t b2, int line) {
            newCode.push_back(b0);
            newCode.push_back(b1);
            newCode.push_back(b2);
            newLines.push_back(line);
            newLines.push_back(line);
            newLines.push_back(line);
            };

        // ---- Emit loop ----
        size_t pc = 0;
        while (pc < oldCode.size()) {
            map[pc] = newCode.size();
            size_t thisStart = pc;

            size_t l = insLen(oldCode, pc);
            if (l == 0) break;

            // Rule 1: CONST a; CONST b; <binop>
            if (pc + 7 <= oldCode.size() &&
                static_cast<OpCode>(oldCode[pc]) == OpCode::CONST &&
                static_cast<OpCode>(oldCode[pc + 3]) == OpCode::CONST) {
                int ai = ((int)oldCode[pc + 1] << 8) | (int)oldCode[pc + 2];
                int bi = ((int)oldCode[pc + 4] << 8) | (int)oldCode[pc + 5];
                OpCode op2 = static_cast<OpCode>(oldCode[pc + 6]);
                if (ai >= 0 && (size_t)ai < chunk.constants.size() &&
                    bi >= 0 && (size_t)bi < chunk.constants.size()) {
                    Value out;
                    if (foldBinaryOp(chunk.constants[ai], chunk.constants[bi], op2, out)) {
                        int newIdx = chunk.addConstant(std::move(out));
                        emit3((uint8_t)OpCode::CONST,
                            (uint8_t)((newIdx >> 8) & 0xFF),
                            (uint8_t)(newIdx & 0xFF),
                            oldLines[pc]);
                        stats.constantsFolded += 1;
                        pc += 7;
                        nextSrc[thisStart] = pc;
                        continue;
                    }
                }
            }

            // Rule 2: CONST a; NEG / NOT
            if (pc + 4 <= oldCode.size() &&
                static_cast<OpCode>(oldCode[pc]) == OpCode::CONST) {
                int ai = ((int)oldCode[pc + 1] << 8) | (int)oldCode[pc + 2];
                OpCode op2 = static_cast<OpCode>(oldCode[pc + 3]);
                if ((op2 == OpCode::NEG || op2 == OpCode::NOT) &&
                    ai >= 0 && (size_t)ai < chunk.constants.size()) {
                    Value out;
                    if (foldUnaryOp(chunk.constants[ai], op2, out)) {
                        int newIdx = chunk.addConstant(std::move(out));
                        emit3((uint8_t)OpCode::CONST,
                            (uint8_t)((newIdx >> 8) & 0xFF),
                            (uint8_t)(newIdx & 0xFF),
                            oldLines[pc]);
                        stats.constantsFolded += 1;
                        pc += 4;
                        nextSrc[thisStart] = pc;
                        continue;
                    }
                }
            }

            // Rule 3: NOT; NOT  ->  nothing
            if (pc + 2 <= oldCode.size() &&
                static_cast<OpCode>(oldCode[pc]) == OpCode::NOT &&
                static_cast<OpCode>(oldCode[pc + 1]) == OpCode::NOT) {
                stats.notNotCollapsed += 1;
                pc += 2;
                nextSrc[thisStart] = pc;
                continue;
            }

            // Default: copy the instruction verbatim.
            for (size_t i = 0; i < l && pc + i < oldCode.size(); ++i) {
                newCode.push_back(oldCode[pc + i]);
                newLines.push_back(oldLines[pc + i]);
            }
            pc += l;
            nextSrc[thisStart] = pc;
        }
        map[oldCode.size()] = newCode.size();

        // Safety: ensure sizes match before disassembly / execution.
        if (newLines.size() != newCode.size()) {
            // This should never happen now, but guard against future bugs.
            newLines.resize(newCode.size(), 0);
        }

        // ---- Retarget jumps ----
        auto mapOffset = [&](size_t srcOff) -> size_t {
            if (srcOff >= oldCode.size()) return newCode.size();
            auto it = map.find(srcOff);
            if (it != map.end()) return it->second;
            // Snap forward to the next mapped source offset.
            for (size_t o = srcOff + 1; o <= oldCode.size(); ++o) {
                auto it2 = map.find(o);
                if (it2 != map.end()) return it2->second;
            }
            return newCode.size();
            };

        size_t srcPc = 0;
        while (srcPc < oldCode.size()) {
            auto it = map.find(srcPc);
            if (it == map.end()) break;
            size_t newPc = it->second;

            OpCode op = static_cast<OpCode>(oldCode[srcPc]);
            bool isJump = (op == OpCode::JUMP ||
                op == OpCode::JUMP_IF_FALSE ||
                op == OpCode::JUMP_IF_TRUE ||
                op == OpCode::ITER_NEXT ||
                op == OpCode::TRY_BEGIN);

            if (isJump && srcPc + 3 <= oldCode.size() &&
                newPc + 3 <= newCode.size()) {
                int16_t oldOff = (int16_t)(((uint16_t)oldCode[srcPc + 1] << 8) |
                    (uint16_t)oldCode[srcPc + 2]);
                size_t oldTarget = (size_t)((int)(srcPc + 3) + (int)oldOff);
                size_t newTarget = mapOffset(oldTarget);
                int newOff = (int)newTarget - (int)(newPc + 3);
                newCode[newPc + 1] = (uint8_t)((newOff >> 8) & 0xFF);
                newCode[newPc + 2] = (uint8_t)(newOff & 0xFF);
            }

            auto nit = nextSrc.find(srcPc);
            if (nit == nextSrc.end()) break;
            srcPc = nit->second;
        }

        chunk.code = std::move(newCode);
        chunk.lines = std::move(newLines);
    }

    void optimizeChunk(Chunk& chunk) {
        OptStats ignored;
        optimizeChunk(chunk, ignored);
    }

} // namespace vayu