#include "VirInterp.hpp"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>

namespace vib {

    namespace {

        struct RtValue {
            VirValueKind kind = VirValueKind::None;
            bool         b = false;
            long long    i = 0;
            double       f = 0.0;
            std::string  s;
            // Phase 7.1/7.2: shared collection payloads.
            std::shared_ptr<std::vector<RtValue>> list;
            std::shared_ptr<std::vector<std::pair<RtValue, RtValue>>> map; // ordered pairs
            std::shared_ptr<std::vector<RtValue>> setElems;
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
            case VirValueKind::List:  return v.list && !v.list->empty();
            case VirValueKind::Map:   return v.map && !v.map->empty();
            case VirValueKind::Tuple: return v.list && !v.list->empty();
            case VirValueKind::Set:   return v.setElems && !v.setElems->empty();
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
            case VirValueKind::List: {
                std::string o = "[";
                if (v.list) {
                    for (size_t i = 0; i < v.list->size(); ++i) {
                        if (i) o += ", ";
                        o += rtToString((*v.list)[i]);
                    }
                }
                o += "]";
                return o;
            }
            case VirValueKind::Map: {
                std::string o = "{";
                if (v.map) {
                    for (size_t i = 0; i < v.map->size(); ++i) {
                        if (i) o += ", ";
                        o += rtToString((*v.map)[i].first) + ": " +
                             rtToString((*v.map)[i].second);
                    }
                }
                o += "}";
                return o;
            }
            case VirValueKind::Tuple: {
                std::string o = "(";
                if (v.list) {
                    for (size_t i = 0; i < v.list->size(); ++i) {
                        if (i) o += ", ";
                        o += rtToString((*v.list)[i]);
                    }
                    if (v.list->size() == 1) o += ",";
                }
                o += ")";
                return o;
            }
            case VirValueKind::Set: {
                std::string o = "{";
                if (v.setElems) {
                    for (size_t i = 0; i < v.setElems->size(); ++i) {
                        if (i) o += ", ";
                        o += rtToString((*v.setElems)[i]);
                    }
                }
                o += "}";
                return o;
            }
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

            // Phase 6.1: instruction-index jumps (targetBlock = pc within this block).
            for (size_t pc = 0; pc < block.instructions.size(); ) {
                const auto& instr = block.instructions[pc];
                size_t nextPc = pc + 1;
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

                case VirOpcode::Pow: {
                    const RtValue& a = locals[(size_t)instr.lhsReg];
                    const RtValue& b = locals[(size_t)instr.rhsReg];
                    RtValue r;
                    if (a.kind == VirValueKind::Int && b.kind == VirValueKind::Int) {
                        if (b.i < 0) {
                            out.kind = ErrorKind::RuntimeError;
                            out.message = "interp: negative integer exponent not supported";
                            return out;
                        }
                        long long base = a.i, exp = b.i, acc = 1;
                        while (exp > 0) {
                            if (exp & 1) acc = (long long)((unsigned long long)acc * (unsigned long long)base);
                            base = (long long)((unsigned long long)base * (unsigned long long)base);
                            exp >>= 1;
                        }
                        r.kind = VirValueKind::Int;
                        r.i = acc;
                    }
                    else if ((a.kind == VirValueKind::Int || a.kind == VirValueKind::Float) &&
                             (b.kind == VirValueKind::Int || b.kind == VirValueKind::Float)) {
                        double x = (a.kind == VirValueKind::Int) ? (double)a.i : a.f;
                        double y = (b.kind == VirValueKind::Int) ? (double)b.i : b.f;
                        r.kind = VirValueKind::Float;
                        r.f = std::pow(x, y);
                    }
                    else {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: ** on non-numeric operands";
                        return out;
                    }
                    locals[(size_t)instr.destReg] = r;
                    break;
                }

                case VirOpcode::LogAnd: {
                    RtValue r;
                    r.kind = VirValueKind::Bool;
                    r.b = truthy(locals[(size_t)instr.lhsReg]) &&
                          truthy(locals[(size_t)instr.rhsReg]);
                    locals[(size_t)instr.destReg] = r;
                    break;
                }
                case VirOpcode::LogOr: {
                    RtValue r;
                    r.kind = VirValueKind::Bool;
                    r.b = truthy(locals[(size_t)instr.lhsReg]) ||
                          truthy(locals[(size_t)instr.rhsReg]);
                    locals[(size_t)instr.destReg] = r;
                    break;
                }
                case VirOpcode::LogNot: {
                    RtValue r;
                    r.kind = VirValueKind::Bool;
                    r.b = !truthy(locals[(size_t)instr.operandReg]);
                    locals[(size_t)instr.destReg] = r;
                    break;
                }
                case VirOpcode::StrRepeat: {
                    const RtValue& a = locals[(size_t)instr.lhsReg];
                    const RtValue& b = locals[(size_t)instr.rhsReg];
                    std::string s;
                    long long n = 0;
                    if (a.kind == VirValueKind::Str && b.kind == VirValueKind::Int) {
                        s = a.s; n = b.i;
                    } else if (a.kind == VirValueKind::Int && b.kind == VirValueKind::Str) {
                        s = b.s; n = a.i;
                    } else {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: string * requires str and int";
                        return out;
                    }
                    if (n < 0) n = 0;
                    if (n > 1000000) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: string repeat count too large";
                        return out;
                    }
                    std::string outS;
                    outS.reserve((size_t)n * s.size());
                    for (long long i = 0; i < n; ++i) outS += s;
                    RtValue r;
                    r.kind = VirValueKind::Str;
                    r.s = std::move(outS);
                    locals[(size_t)instr.destReg] = std::move(r);
                    break;
                }


                case VirOpcode::ListNew: {
                    RtValue r;
                    r.kind = VirValueKind::List;
                    r.list = std::make_shared<std::vector<RtValue>>();
                    locals[(size_t)instr.destReg] = std::move(r);
                    break;
                }
                case VirOpcode::ListPush: {
                    RtValue& lst = locals[(size_t)instr.lhsReg];
                    if (lst.kind != VirValueKind::List || !lst.list) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: ListPush on non-list";
                        return out;
                    }
                    lst.list->push_back(locals[(size_t)instr.operandReg]);
                    break;
                }
                case VirOpcode::ListGet: {
                    const RtValue& lst = locals[(size_t)instr.lhsReg];
                    const RtValue& ix = locals[(size_t)instr.rhsReg];
                    if ((lst.kind != VirValueKind::List && lst.kind != VirValueKind::Tuple) || !lst.list) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: index on non-list/tuple";
                        return out;
                    }
                    if (ix.kind != VirValueKind::Int) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: list index must be int";
                        return out;
                    }
                    long long i = ix.i;
                    if (i < 0) i += (long long)lst.list->size();
                    if (i < 0 || i >= (long long)lst.list->size()) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: list index out of range";
                        return out;
                    }
                    locals[(size_t)instr.destReg] = (*lst.list)[(size_t)i];
                    break;
                }
                case VirOpcode::ListSet: {
                    RtValue& lst = locals[(size_t)instr.lhsReg];
                    const RtValue& ix = locals[(size_t)instr.rhsReg];
                    if (lst.kind != VirValueKind::List || !lst.list) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: list set on non-list";
                        return out;
                    }
                    if (ix.kind != VirValueKind::Int) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: list index must be int";
                        return out;
                    }
                    long long i = ix.i;
                    if (i < 0) i += (long long)lst.list->size();
                    if (i < 0 || i >= (long long)lst.list->size()) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: list index out of range";
                        return out;
                    }
                    (*lst.list)[(size_t)i] = locals[(size_t)instr.operandReg];
                    break;
                }
                case VirOpcode::ListLen: {
                    const RtValue& lst = locals[(size_t)instr.operandReg];
                    if (lst.kind != VirValueKind::List || !lst.list) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: ListLen on non-list";
                        return out;
                    }
                    RtValue r;
                    r.kind = VirValueKind::Int;
                    r.i = (long long)lst.list->size();
                    locals[(size_t)instr.destReg] = r;
                    break;
                }

                
                case VirOpcode::MapNew: {
                    RtValue r; r.kind = VirValueKind::Map;
                    r.map = std::make_shared<std::vector<std::pair<RtValue, RtValue>>>();
                    locals[(size_t)instr.destReg] = std::move(r);
                    break;
                }
                case VirOpcode::MapSet: {
                    RtValue& m = locals[(size_t)instr.lhsReg];
                    if (m.kind != VirValueKind::Map || !m.map) {
                        out.kind = ErrorKind::RuntimeError; out.message = "interp: MapSet on non-map"; return out;
                    }
                    const RtValue& key = locals[(size_t)instr.rhsReg];
                    const RtValue& val = locals[(size_t)instr.operandReg];
                    bool found = false;
                    for (auto& kv : *m.map) {
                        if (kv.first.kind == key.kind &&
                            ((key.kind == VirValueKind::Str && kv.first.s == key.s) ||
                             (key.kind == VirValueKind::Int && kv.first.i == key.i) ||
                             (key.kind == VirValueKind::Bool && kv.first.b == key.b))) {
                            kv.second = val; found = true; break;
                        }
                    }
                    if (!found) m.map->push_back({key, val});
                    break;
                }
                case VirOpcode::MapGet: {
                    const RtValue& m = locals[(size_t)instr.lhsReg];
                    const RtValue& key = locals[(size_t)instr.rhsReg];
                    if (m.kind != VirValueKind::Map || !m.map) {
                        out.kind = ErrorKind::RuntimeError; out.message = "interp: MapGet on non-map"; return out;
                    }
                    bool found = false;
                    for (const auto& kv : *m.map) {
                        if (kv.first.kind == key.kind &&
                            ((key.kind == VirValueKind::Str && kv.first.s == key.s) ||
                             (key.kind == VirValueKind::Int && kv.first.i == key.i) ||
                             (key.kind == VirValueKind::Bool && kv.first.b == key.b))) {
                            locals[(size_t)instr.destReg] = kv.second;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        RtValue none; none.kind = VirValueKind::None;
                        locals[(size_t)instr.destReg] = none;
                    }
                    break;
                }
                case VirOpcode::MapHas: {
                    const RtValue& m = locals[(size_t)instr.lhsReg];
                    const RtValue& key = locals[(size_t)instr.rhsReg];
                    RtValue r; r.kind = VirValueKind::Bool; r.b = false;
                    if (m.kind == VirValueKind::Map && m.map) {
                        for (const auto& kv : *m.map) {
                            if (kv.first.kind == key.kind &&
                                ((key.kind == VirValueKind::Str && kv.first.s == key.s) ||
                                 (key.kind == VirValueKind::Int && kv.first.i == key.i)))
                            { r.b = true; break; }
                        }
                    }
                    locals[(size_t)instr.destReg] = r;
                    break;
                }
                case VirOpcode::TupleNew: {
                    RtValue r; r.kind = VirValueKind::Tuple;
                    r.list = std::make_shared<std::vector<RtValue>>();
                    for (int ar : instr.argRegs)
                        r.list->push_back(locals[(size_t)ar]);
                    locals[(size_t)instr.destReg] = std::move(r);
                    break;
                }
                case VirOpcode::SetNew: {
                    RtValue r; r.kind = VirValueKind::Set;
                    r.setElems = std::make_shared<std::vector<RtValue>>();
                    locals[(size_t)instr.destReg] = std::move(r);
                    break;
                }
                case VirOpcode::SetAdd: {
                    RtValue& s = locals[(size_t)instr.lhsReg];
                    if (s.kind != VirValueKind::Set || !s.setElems) {
                        out.kind = ErrorKind::RuntimeError; out.message = "interp: SetAdd on non-set"; return out;
                    }
                    const RtValue& el = locals[(size_t)instr.operandReg];
                    bool found = false;
                    for (const auto& e : *s.setElems) {
                        if (e.kind == el.kind &&
                            ((el.kind == VirValueKind::Int && e.i == el.i) ||
                             (el.kind == VirValueKind::Str && e.s == el.s)))
                        { found = true; break; }
                    }
                    if (!found) s.setElems->push_back(el);
                    break;
                }
                case VirOpcode::SetHas: {
                    const RtValue& s = locals[(size_t)instr.lhsReg];
                    const RtValue& el = locals[(size_t)instr.rhsReg];
                    RtValue r; r.kind = VirValueKind::Bool; r.b = false;
                    if (s.kind == VirValueKind::Set && s.setElems) {
                        for (const auto& e : *s.setElems) {
                            if (e.kind == el.kind &&
                                ((el.kind == VirValueKind::Int && e.i == el.i) ||
                                 (el.kind == VirValueKind::Str && e.s == el.s)))
                            { r.b = true; break; }
                        }
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
                    std::vector<RtValue> callArgs;
                    callArgs.reserve(instr.argRegs.size());
                    for (int r : instr.argRegs) callArgs.push_back(locals[(size_t)r]);
                    RtValue r;
                    switch (instr.builtin) {
                    case VirBuiltin::Print: {
                        std::string line;
                        for (size_t i = 0; i < callArgs.size(); ++i) {
                            if (i) line += " ";
                            line += rtToString(callArgs[i]);
                        }
                        stdoutLines.push_back(line);
                        r.kind = VirValueKind::None;
                        break;
                    }
                    case VirBuiltin::ToStr: {
                        r.kind = VirValueKind::Str;
                        r.s = rtToString(callArgs[0]);
                        break;
                    }
                    case VirBuiltin::ToInt: {
                        const RtValue& v = callArgs[0];
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
                        const RtValue& v = callArgs[0];
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
                        r.b = truthy(callArgs[0]);
                        break;
                    }
                    case VirBuiltin::LenStr: {
                        r.kind = VirValueKind::Int;
                        if (callArgs[0].kind == VirValueKind::Str) {
                            r.i = (long long)callArgs[0].s.size();
                        } else if (callArgs[0].kind == VirValueKind::List && callArgs[0].list) {
                            r.i = (long long)callArgs[0].list->size();
                        } else if (callArgs[0].kind == VirValueKind::Map && callArgs[0].map) {
                            r.i = (long long)callArgs[0].map->size();
                        } else if (callArgs[0].kind == VirValueKind::Tuple && callArgs[0].list) {
                            r.i = (long long)callArgs[0].list->size();
                        } else if (callArgs[0].kind == VirValueKind::Set && callArgs[0].setElems) {
                            r.i = (long long)callArgs[0].setElems->size();
                        } else {
                            out.kind = ErrorKind::RuntimeError; out.message = "interp: len() expects str/list/map/tuple/set"; return out;
                        }
                        break;
                    }
                    case VirBuiltin::StrUpper:
                    case VirBuiltin::StrLower: {
                        r.kind = VirValueKind::Str;
                        std::string s = callArgs[0].s;
                        for (auto& ch : s) {
                            if (instr.builtin == VirBuiltin::StrUpper) ch = (char)std::toupper((unsigned char)ch);
                            else                                        ch = (char)std::tolower((unsigned char)ch);
                        }
                        r.s = std::move(s);
                        break;
                    }
                    case VirBuiltin::StrStrip: {
                        const std::string& s = callArgs[0].s;
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
                        r.b = callArgs[0].s.find(callArgs[1].s) != std::string::npos;
                        break;
                    }
                    case VirBuiltin::StrFind: {
                        r.kind = VirValueKind::Int;
                        size_t p = callArgs[0].s.find(callArgs[1].s);
                        r.i = (p == std::string::npos) ? -1 : (long long)p;
                        break;
                    }
                    case VirBuiltin::StrStartsWith: {
                        r.kind = VirValueKind::Bool;
                        r.b = callArgs[0].s.size() >= callArgs[1].s.size() &&
                            callArgs[0].s.compare(0, callArgs[1].s.size(), callArgs[1].s) == 0;
                        break;
                    }
                    case VirBuiltin::StrEndsWith: {
                        r.kind = VirValueKind::Bool;
                        r.b = callArgs[0].s.size() >= callArgs[1].s.size() &&
                            callArgs[0].s.compare(callArgs[0].s.size() - callArgs[1].s.size(),
                                callArgs[1].s.size(), callArgs[1].s) == 0;
                        break;
                    }
                    case VirBuiltin::StrIsDigit: {
                        r.kind = VirValueKind::Bool;
                        r.b = !callArgs[0].s.empty();
                        for (char ch : callArgs[0].s) if (!std::isdigit((unsigned char)ch)) { r.b = false; break; }
                        break;
                    }
                    case VirBuiltin::StrIsAlpha: {
                        r.kind = VirValueKind::Bool;
                        r.b = !callArgs[0].s.empty();
                        for (char ch : callArgs[0].s) if (!std::isalpha((unsigned char)ch)) { r.b = false; break; }
                        break;
                    }
                    case VirBuiltin::StrIsSpace: {
                        r.kind = VirValueKind::Bool;
                        r.b = !callArgs[0].s.empty();
                        auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
                        for (char ch : callArgs[0].s) if (!isws(ch)) { r.b = false; break; }
                        break;
                    }
                    case VirBuiltin::StrReplace: {
                        const std::string& hay = callArgs[0].s;
                        const std::string& oldS = callArgs[1].s;
                        const std::string& newS = callArgs[2].s;
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
                        const std::string& s = callArgs[0].s;
                        long long i = callArgs[1].i;
                        if (i < 0) i += (long long)s.size();
                        if (i < 0 || i >= (long long)s.size()) { out.kind = ErrorKind::RuntimeError; out.message = "interp: char_at out of range"; return out; }
                        r.kind = VirValueKind::Str;
                        r.s = std::string(1, s[(size_t)i]);
                        break;
                    }
                    case VirBuiltin::StrToInt: {
                        r.kind = VirValueKind::Int;
                        try { r.i = std::stoll(callArgs[0].s); }
                        catch (...) { out.kind = ErrorKind::RuntimeError; out.message = "interp: to_int() parse error on \"" + callArgs[0].s + "\""; return out; }
                        break;
                    }
                    }
                    locals[(size_t)instr.destReg] = std::move(r);
                    break;
                }

                case VirOpcode::Jump: {
                    if (instr.targetBlock < 0 ||
                        instr.targetBlock > (int)block.instructions.size()) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: Jump target out of range";
                        return out;
                    }
                    nextPc = (size_t)instr.targetBlock;
                    break;
                }
                case VirOpcode::JumpIfFalse: {
                    if (instr.targetBlock < 0 ||
                        instr.targetBlock > (int)block.instructions.size()) {
                        out.kind = ErrorKind::RuntimeError;
                        out.message = "interp: JumpIfFalse target out of range";
                        return out;
                    }
                    if (!truthy(locals[(size_t)instr.operandReg]))
                        nextPc = (size_t)instr.targetBlock;
                    break;
                }
                case VirOpcode::Ret: {
                    out.success = true;
                    out.kind = ErrorKind::None;
                    out.returnValue = locals[(size_t)instr.operandReg];
                    return out;
                }
                }
                pc = nextPc;
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