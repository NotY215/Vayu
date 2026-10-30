#include "VirInterp.hpp"
#include <cctype>
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

        std::string rtToString(const RtValue& v) {
            switch (v.kind) {
            case VirValueKind::None:  return "None";
            case VirValueKind::Bool:  return v.b ? "true" : "false";
            case VirValueKind::Int:   return std::to_string(v.i);
            case VirValueKind::Float: {
                std::ostringstream oss;
                oss.precision(15);
                oss << v.f;
                std::string s = oss.str();
                if (s.find('.') == std::string::npos &&
                    s.find('e') == std::string::npos &&
                    s.find("inf") == std::string::npos &&
                    s.find("nan") == std::string::npos) s += ".0";
                return s;
            }
            case VirValueKind::Str:   return v.s;
            }
            return "?";
        }

        struct CallOutcome {
            bool         success = false;
            ErrorKind    kind = ErrorKind::None;
            std::string  message;
            RtValue      returnValue;
        };

        // Forward declaration for mutual recursion.
        CallOutcome executeFunction(const VirModule& module,
            const VirFunction& fn,
            std::vector<RtValue> args,
            std::vector<std::string>& stdoutLines,
            int depth);

        CallOutcome executeFunction(const VirModule& module,
            const VirFunction& fn,
            std::vector<RtValue> args,
            std::vector<std::string>& stdoutLines,
            int depth) {
            CallOutcome out;
            out.returnValue.kind = VirValueKind::None;

            if (depth > 4096) {
                out.kind = ErrorKind::RuntimeError;
                out.message = "interp: call depth exceeded (max 4096)";
                return out;
            }
            if (args.size() != fn.paramNames.size()) {
                out.kind = ErrorKind::RuntimeError;
                out.message = "interp: function \"" + fn.name + "\" expects " +
                    std::to_string(fn.paramNames.size()) + " arg(s), got " +
                    std::to_string(args.size());
                return out;
            }
            if (fn.blocks.empty()) {
                out.kind = ErrorKind::RuntimeError;
                out.message = "interp: function \"" + fn.name + "\" has no blocks";
                return out;
            }

            std::vector<RtValue> locals((size_t)fn.localCount);
            for (size_t i = 0; i < args.size(); ++i) locals[i] = std::move(args[i]);

            const VirBlock& block = fn.blocks[(size_t)fn.entryBlock];

            for (const auto& instr : block.instructions) {
                switch (instr.opcode) {
                case VirOpcode::Nop:
                    break;

                case VirOpcode::Const: {
                    if (instr.constIndex < 0 ||
                        instr.constIndex >= (int)module.constants.size()) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: invalid constIndex";
                        return out;
                    }
                    locals[(size_t)instr.destReg] =
                        fromVir(module.constants[instr.constIndex]);
                    break;
                }

                case VirOpcode::LoadLocal: {
                    if (instr.localIndex < 0 || instr.localIndex >= fn.localCount) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: invalid LoadLocal index";
                        return out;
                    }
                    locals[(size_t)instr.destReg] = locals[(size_t)instr.localIndex];
                    break;
                }

                case VirOpcode::StoreLocal: {
                    if (instr.localIndex < 0 || instr.localIndex >= fn.localCount) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: invalid StoreLocal index";
                        return out;
                    }
                    locals[(size_t)instr.localIndex] = locals[(size_t)instr.operandReg];
                    break;
                }

                case VirOpcode::Add: case VirOpcode::Sub: case VirOpcode::Mul:
                case VirOpcode::Div: case VirOpcode::Mod: {
                    const RtValue& a = locals[(size_t)instr.lhsReg];
                    const RtValue& b = locals[(size_t)instr.rhsReg];
                    RtValue r;
                    bool ok = true;
                    if (a.kind == VirValueKind::Int && b.kind == VirValueKind::Int) {
                        r.kind = VirValueKind::Int;
                        long long x = a.i, y = b.i;
                        switch (instr.opcode) {
                        case VirOpcode::Add: r.i = (long long)((unsigned long long)x + (unsigned long long)y); break;
                        case VirOpcode::Sub: r.i = (long long)((unsigned long long)x - (unsigned long long)y); break;
                        case VirOpcode::Mul: r.i = (long long)((unsigned long long)x * (unsigned long long)y); break;
                        case VirOpcode::Div:
                            if (y == 0) { ok = false; out.message = "interp: integer division by zero"; }
                            else if (y == -1 && x == std::numeric_limits<long long>::min())
                                r.i = std::numeric_limits<long long>::min();
                            else r.i = x / y;
                            break;
                        case VirOpcode::Mod:
                            if (y == 0) { ok = false; out.message = "interp: integer modulo by zero"; }
                            else if (y == -1) r.i = 0;
                            else r.i = x % y;
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
                            if (y == 0.0) { ok = false; out.message = "interp: division by zero"; }
                            else r.f = x / y;
                            break;
                        case VirOpcode::Mod:
                            ok = false; out.message = "interp: '%' on floats is Phase 6+";
                            break;
                        default: break;
                        }
                    }
                    else {
                        ok = false;
                        out.message = "interp: arithmetic on non-numeric operands";
                    }
                    if (!ok) { out.kind = ErrorKind::RuntimeError; return out; }
                    locals[(size_t)instr.destReg] = std::move(r);
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
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: unary '-' on non-numeric operand";
                        return out;
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
                        case VirOpcode::CmpEq: res = true;  break;
                        case VirOpcode::CmpNe: res = false; break;
                        default: break;
                        }
                    }
                    else {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: comparison on incompatible operand kinds";
                        return out;
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
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: StrConcat requires two string operands";
                        return out;
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
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: string comparison requires two string operands";
                        return out;
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

                case VirOpcode::Call: {
                    if (instr.funcIndex < 0 ||
                        instr.funcIndex >= (int)module.functions.size()) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: Call funcIndex out of range";
                        return out;
                    }
                    std::vector<RtValue> callArgs;
                    callArgs.reserve(instr.argRegs.size());
                    for (int r : instr.argRegs) callArgs.push_back(locals[(size_t)r]);
                    CallOutcome sub = executeFunction(
                        module, module.functions[(size_t)instr.funcIndex],
                        std::move(callArgs), stdoutLines, depth + 1);
                    if (!sub.success) {
                        out.kind = sub.kind;
                        out.message = sub.message;
                        return out;
                    }
                    // Phase 5.2b: return value is plumbed back.
                    locals[(size_t)instr.destReg] = sub.returnValue;
                    break;
                }

                case VirOpcode::CallBuiltin: {
                    std::vector<RtValue> args;
                    args.reserve(instr.argRegs.size());
                    for (int r : instr.argRegs) args.push_back(locals[(size_t)r]);
                    RtValue r;
                    switch (instr.builtin) {
                    case VirBuiltin::Print: {
                        std::string line;
                        for (size_t i = 0; i < args.size(); ++i) {
                            if (i) line += " ";
                            line += rtToString(args[i]);
                        }
                        stdoutLines.push_back(line);
                        r.kind = VirValueKind::None;
                        break;
                    }
                    case VirBuiltin::ToStr: {
                        r.kind = VirValueKind::Str;
                        r.s = rtToString(args[0]);
                        break;
                    }
                    case VirBuiltin::ToInt: {
                        const RtValue& v = args[0];
                        r.kind = VirValueKind::Int;
                        if (v.kind == VirValueKind::Int)        r.i = v.i;
                        else if (v.kind == VirValueKind::Float) r.i = (long long)v.f;
                        else if (v.kind == VirValueKind::Bool)  r.i = v.b ? 1 : 0;
                        else if (v.kind == VirValueKind::Str) {
                            try { r.i = std::stoll(v.s); }
                            catch (...) { out.kind = ErrorKind::RuntimeError; out.message = "interp: int() parse error on \"" + v.s + "\""; return out; }
                        }
                        else {
                            out.kind = ErrorKind::RuntimeError; out.message = "interp: int() on None"; return out;
                        }
                        break;
                    }
                    case VirBuiltin::ToFloat: {
                        const RtValue& v = args[0];
                        r.kind = VirValueKind::Float;
                        if (v.kind == VirValueKind::Int)        r.f = (double)v.i;
                        else if (v.kind == VirValueKind::Float) r.f = v.f;
                        else if (v.kind == VirValueKind::Bool)  r.f = v.b ? 1.0 : 0.0;
                        else if (v.kind == VirValueKind::Str) {
                            try { r.f = std::stod(v.s); }
                            catch (...) { out.kind = ErrorKind::RuntimeError; out.message = "interp: float() parse error on \"" + v.s + "\""; return out; }
                        }
                        else {
                            out.kind = ErrorKind::RuntimeError; out.message = "interp: float() on None"; return out;
                        }
                        break;
                    }
                    case VirBuiltin::ToBool: {
                        r.kind = VirValueKind::Bool;
                        r.b = truthy(args[0]);
                        break;
                    }
                    case VirBuiltin::LenStr: {
                        if (args[0].kind != VirValueKind::Str) {
                            out.kind = ErrorKind::RuntimeError; out.message = "interp: len() on non-str in Phase 5"; return out;
                        }
                        r.kind = VirValueKind::Int;
                        r.i = (long long)args[0].s.size();
                        break;
                    }
                    case VirBuiltin::StrUpper:
                    case VirBuiltin::StrLower: {
                        r.kind = VirValueKind::Str;
                        std::string s = args[0].s;
                        for (auto& ch : s) {
                            if (instr.builtin == VirBuiltin::StrUpper) ch = (char)std::toupper((unsigned char)ch);
                            else                                        ch = (char)std::tolower((unsigned char)ch);
                        }
                        r.s = std::move(s);
                        break;
                    }
                    case VirBuiltin::StrStrip: {
                        const std::string& s = args[0].s;
                        size_t a = 0, b = s.size();
                        auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
                        while (a < b && isws(s[a])) ++a;
                        while (b > a && isws(s[b - 1])) --b;
                        r.kind = VirValueKind::Str;
                        r.s = s.substr(a, b - a);
                        break;
                    }
                    case VirBuiltin::StrContains: {
                        r.kind = VirValueKind::Bool;
                        r.b = args[0].s.find(args[1].s) != std::string::npos;
                        break;
                    }
                    case VirBuiltin::StrFind: {
                        r.kind = VirValueKind::Int;
                        size_t p = args[0].s.find(args[1].s);
                        r.i = (p == std::string::npos) ? -1 : (long long)p;
                        break;
                    }
                    case VirBuiltin::StrStartsWith: {
                        r.kind = VirValueKind::Bool;
                        r.b = args[0].s.size() >= args[1].s.size() &&
                            args[0].s.compare(0, args[1].s.size(), args[1].s) == 0;
                        break;
                    }
                    case VirBuiltin::StrEndsWith: {
                        r.kind = VirValueKind::Bool;
                        r.b = args[0].s.size() >= args[1].s.size() &&
                            args[0].s.compare(args[0].s.size() - args[1].s.size(),
                                args[1].s.size(), args[1].s) == 0;
                        break;
                    }
                    case VirBuiltin::StrIsDigit: {
                        r.kind = VirValueKind::Bool;
                        r.b = !args[0].s.empty();
                        for (char ch : args[0].s) if (!std::isdigit((unsigned char)ch)) { r.b = false; break; }
                        break;
                    }
                    case VirBuiltin::StrIsAlpha: {
                        r.kind = VirValueKind::Bool;
                        r.b = !args[0].s.empty();
                        for (char ch : args[0].s) if (!std::isalpha((unsigned char)ch)) { r.b = false; break; }
                        break;
                    }
                    case VirBuiltin::StrIsSpace: {
                        r.kind = VirValueKind::Bool;
                        r.b = !args[0].s.empty();
                        auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
                        for (char ch : args[0].s) if (!isws(ch)) { r.b = false; break; }
                        break;
                    }
                    case VirBuiltin::StrReplace: {
                        const std::string& hay = args[0].s;
                        const std::string& oldS = args[1].s;
                        const std::string& newS = args[2].s;
                        r.kind = VirValueKind::Str;
                        if (oldS.empty()) { r.s = hay; break; }
                        std::string outS;
                        size_t pos = 0, next;
                        while ((next = hay.find(oldS, pos)) != std::string::npos) {
                            outS += hay.substr(pos, next - pos);
                            outS += newS;
                            pos = next + oldS.size();
                        }
                        outS += hay.substr(pos);
                        r.s = std::move(outS);
                        break;
                    }
                    case VirBuiltin::StrCharAt: {
                        const std::string& s = args[0].s;
                        long long i = args[1].i;
                        if (i < 0) i += (long long)s.size();
                        if (i < 0 || i >= (long long)s.size()) { out.kind = ErrorKind::RuntimeError; out.message = "interp: char_at out of range"; return out; }
                        r.kind = VirValueKind::Str;
                        r.s = std::string(1, s[(size_t)i]);
                        break;
                    }
                    case VirBuiltin::StrToInt: {
                        r.kind = VirValueKind::Int;
                        try { r.i = std::stoll(args[0].s); }
                        catch (...) { out.kind = ErrorKind::RuntimeError; out.message = "interp: to_int() parse error on \"" + args[0].s + "\""; return out; }
                        break;
                    }
                    }
                    locals[(size_t)instr.destReg] = std::move(r);
                    break;
                }

                case VirOpcode::Ret: {
                    out.success = true;
                    out.kind = ErrorKind::None;
                    out.returnValue = locals[(size_t)instr.operandReg];
                    return out;
                }
                }
            }

            out.kind = ErrorKind::RuntimeError;
            out.message = "interp: block ended without Ret";
            return out;
        }

    } // namespace

    VirExecResult VirInterpreter::execute(const VirModule& module) const {
        VirExecResult out;
        if (module.functions.empty()) {
            out.kind = ErrorKind::RuntimeError;
            out.message = "interp: module has no functions";
            return out;
        }
        if (module.entryFunction < 0 ||
            module.entryFunction >= (int)module.functions.size()) {
            out.kind = ErrorKind::RuntimeError;
            out.message = "interp: entryFunction index out of range";
            return out;
        }
        CallOutcome co = executeFunction(
            module, module.functions[(size_t)module.entryFunction],
            {}, out.stdoutLines, 0);
        out.success = co.success;
        out.kind = co.kind;
        out.message = co.message;
        out.exitCode = 0;
        return out;
    }

} // namespace vib