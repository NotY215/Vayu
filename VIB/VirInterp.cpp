#include "VirInterp.hpp"
#include <cstdint>
#include <limits>
#include <sstream>

namespace vib {

    namespace {

        struct RtValue {
            VirValueKind kind = VirValueKind::None;
            bool         b = false;
            long long    i = 0;
            double       f = 0.0;
            std::string  s;
        };

        RtValue fromVir(const VirValue& v) {
            RtValue r;
            r.kind = v.kind;
            r.b = v.boolValue;
            r.i = v.intValue;
            r.f = v.floatValue;
            r.s = v.strValue;
            return r;
        }

        bool truthy(const RtValue& v) {
            switch (v.kind) {
            case VirValueKind::None:  return false;
            case VirValueKind::Bool:  return v.b;
            case VirValueKind::Int:   return v.i != 0;
            case VirValueKind::Float: return v.f != 0.0;
            case VirValueKind::Str:   return !v.s.empty();
            }
            return false;
        }

        VirExecResult fail(int plannedPhase, const char* feature, const std::string& msg) {
            VirExecResult r;
            r.success = false;
            r.kind = ErrorKind::RuntimeError;
            r.message = msg;
            (void)plannedPhase;
            (void)feature;
            return r;
        }

    } // namespace

    VirExecResult VirInterpreter::execute(const VirModule& module) const {
        VirExecResult out;

        if (module.functions.empty()) {
            out.kind = ErrorKind::RuntimeError;
            out.message = "interp: module has no functions";
            return out;
        }
        const VirFunction& fn = module.functions[module.entryFunction];
        if (fn.blocks.empty()) {
            out.kind = ErrorKind::RuntimeError;
            out.message = "interp: entry function has no blocks";
            return out;
        }

        std::vector<RtValue> locals((size_t)fn.localCount);

        const VirBlock& block = fn.blocks[fn.entryBlock];
        for (const auto& instr : block.instructions) {
            switch (instr.opcode) {
            case VirOpcode::Nop:
                break;

            case VirOpcode::Const: {
                if (instr.constIndex < 0 ||
                    instr.constIndex >= (int)module.constants.size()) {
                    return fail(0, nullptr, "interp: invalid constIndex");
                }
                locals[(size_t)instr.destReg] = fromVir(module.constants[instr.constIndex]);
                break;
            }

            case VirOpcode::LoadLocal: {
                if (instr.localIndex < 0 || instr.localIndex >= fn.localCount) {
                    return fail(0, nullptr, "interp: invalid LoadLocal index");
                }
                locals[(size_t)instr.destReg] = locals[(size_t)instr.localIndex];
                break;
            }

            case VirOpcode::StoreLocal: {
                if (instr.localIndex < 0 || instr.localIndex >= fn.localCount) {
                    return fail(0, nullptr, "interp: invalid StoreLocal index");
                }
                locals[(size_t)instr.localIndex] = locals[(size_t)instr.operandReg];
                break;
            }

            case VirOpcode::Add: case VirOpcode::Sub: case VirOpcode::Mul:
            case VirOpcode::Div: case VirOpcode::Mod: {
                const RtValue& a = locals[(size_t)instr.lhsReg];
                const RtValue& b = locals[(size_t)instr.rhsReg];
                RtValue r;
                if (a.kind == VirValueKind::Int && b.kind == VirValueKind::Int) {
                    r.kind = VirValueKind::Int;
                    long long x = a.i, y = b.i;
                    switch (instr.opcode) {
                    case VirOpcode::Add: r.i = (long long)((unsigned long long)x + (unsigned long long)y); break;
                    case VirOpcode::Sub: r.i = (long long)((unsigned long long)x - (unsigned long long)y); break;
                    case VirOpcode::Mul: r.i = (long long)((unsigned long long)x * (unsigned long long)y); break;
                    case VirOpcode::Div:
                        if (y == 0) return fail(0, nullptr, "interp: integer division by zero");
                        if (y == -1 && x == std::numeric_limits<long long>::min())
                            r.i = std::numeric_limits<long long>::min();   // wrap
                        else
                            r.i = x / y;
                        break;
                    case VirOpcode::Mod:
                        if (y == 0) return fail(0, nullptr, "interp: integer modulo by zero");
                        if (y == -1) r.i = 0;
                        else         r.i = x % y;
                        break;
                    default: break;
                    }
                }
                else if ((a.kind == VirValueKind::Int || a.kind == VirValueKind::Float) &&
                    (b.kind == VirValueKind::Int || b.kind == VirValueKind::Float)) {
                    double x = (a.kind == VirValueKind::Int) ? (double)a.i : a.f;
                    double y = (b.kind == VirValueKind::Int) ? (double)b.i : b.f;
                    r.kind = VirValueKind::Float;
                    switch (instr.opcode) {
                    case VirOpcode::Add: r.f = x + y; break;
                    case VirOpcode::Sub: r.f = x - y; break;
                    case VirOpcode::Mul: r.f = x * y; break;
                    case VirOpcode::Div:
                        if (y == 0.0) return fail(0, nullptr, "interp: division by zero");
                        r.f = x / y; break;
                    case VirOpcode::Mod:
                        return fail(0, nullptr, "interp: '%' not defined on floats in Phase 3");
                    default: break;
                    }
                }
                else {
                    return fail(0, nullptr, "interp: arithmetic on non-numeric operands");
                }
                locals[(size_t)instr.destReg] = r;
                break;
            }

            case VirOpcode::Neg: {
                const RtValue& a = locals[(size_t)instr.operandReg];
                RtValue r;
                if (a.kind == VirValueKind::Int) {
                    r.kind = VirValueKind::Int;
                    r.i = (long long)(0ULL - (unsigned long long)a.i);
                }
                else if (a.kind == VirValueKind::Float) {
                    r.kind = VirValueKind::Float;
                    r.f = -a.f;
                }
                else {
                    return fail(0, nullptr, "interp: unary '-' on non-numeric operand");
                }
                locals[(size_t)instr.destReg] = r;
                break;
            }

            case VirOpcode::CmpEq: case VirOpcode::CmpNe:
            case VirOpcode::CmpLt: case VirOpcode::CmpLe:
            case VirOpcode::CmpGt: case VirOpcode::CmpGe: {
                const RtValue& a = locals[(size_t)instr.lhsReg];
                const RtValue& b = locals[(size_t)instr.rhsReg];
                bool res = false;
                if (a.kind == VirValueKind::Int && b.kind == VirValueKind::Int) {
                    switch (instr.opcode) {
                    case VirOpcode::CmpEq: res = a.i == b.i; break;
                    case VirOpcode::CmpNe: res = a.i != b.i; break;
                    case VirOpcode::CmpLt: res = a.i < b.i; break;
                    case VirOpcode::CmpLe: res = a.i <= b.i; break;
                    case VirOpcode::CmpGt: res = a.i > b.i; break;
                    case VirOpcode::CmpGe: res = a.i >= b.i; break;
                    default: break;
                    }
                }
                else if ((a.kind == VirValueKind::Int || a.kind == VirValueKind::Float) &&
                    (b.kind == VirValueKind::Int || b.kind == VirValueKind::Float)) {
                    double x = (a.kind == VirValueKind::Int) ? (double)a.i : a.f;
                    double y = (b.kind == VirValueKind::Int) ? (double)b.i : b.f;
                    switch (instr.opcode) {
                    case VirOpcode::CmpEq: res = x == y; break;
                    case VirOpcode::CmpNe: res = x != y; break;
                    case VirOpcode::CmpLt: res = x < y; break;
                    case VirOpcode::CmpLe: res = x <= y; break;
                    case VirOpcode::CmpGt: res = x > y; break;
                    case VirOpcode::CmpGe: res = x >= y; break;
                    default: break;
                    }
                }
                else if (a.kind == VirValueKind::Bool && b.kind == VirValueKind::Bool) {
                    switch (instr.opcode) {
                    case VirOpcode::CmpEq: res = a.b == b.b; break;
                    case VirOpcode::CmpNe: res = a.b != b.b; break;
                    default: break;
                    }
                }
                else if (a.kind == VirValueKind::None && b.kind == VirValueKind::None) {
                    switch (instr.opcode) {
                    case VirOpcode::CmpEq: res = true; break;
                    case VirOpcode::CmpNe: res = false; break;
                    default: break;
                    }
                }
                else {
                    return fail(0, nullptr, "interp: comparison on incompatible operand kinds");
                }
                RtValue r;
                r.kind = VirValueKind::Bool;
                r.b = res;
                locals[(size_t)instr.destReg] = r;
                break;
            }

            case VirOpcode::StrConcat: {
                const RtValue& a = locals[(size_t)instr.lhsReg];
                const RtValue& b = locals[(size_t)instr.rhsReg];
                if (a.kind != VirValueKind::Str || b.kind != VirValueKind::Str) {
                    return fail(0, nullptr, "interp: StrConcat requires two string operands");
                }
                RtValue r;
                r.kind = VirValueKind::Str;
                r.s = a.s + b.s;
                locals[(size_t)instr.destReg] = std::move(r);
                break;
            }

            case VirOpcode::StrEq:
            case VirOpcode::StrNe:
            case VirOpcode::StrCmpLt: {
                const RtValue& a = locals[(size_t)instr.lhsReg];
                const RtValue& b = locals[(size_t)instr.rhsReg];
                if (a.kind != VirValueKind::Str || b.kind != VirValueKind::Str) {
                    return fail(0, nullptr, "interp: string comparison requires two string operands");
                }
                bool res = false;
                switch (instr.opcode) {
                case VirOpcode::StrEq:    res = (a.s == b.s); break;
                case VirOpcode::StrNe:    res = (a.s != b.s); break;
                case VirOpcode::StrCmpLt: res = (a.s < b.s); break;
                default: break;
                }
                RtValue r;
                r.kind = VirValueKind::Bool;
                r.b = res;
                locals[(size_t)instr.destReg] = r;
                break;
            }

            case VirOpcode::Ret: {
                out.success = true;
                out.kind = ErrorKind::None;
                out.exitCode = 0;
                (void)instr.operandReg;   // return value discarded for Phase 3
                return out;
            }
            }
        }

        out.kind = ErrorKind::RuntimeError;
        out.message = "interp: block ended without Ret";
        return out;
    }

} // namespace vib