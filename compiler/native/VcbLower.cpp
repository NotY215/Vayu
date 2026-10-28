#include "VcbLower.hpp"
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace vayu {

    namespace {

        enum class VType : int {
            Int = 0,
            Bool = 1,
            Str = 2,
            Float = 3,
            List = 4,
            Map = 5,
            Tuple = 6,
            Set = 7,
        };

        class Lower {
        public:
            std::string run(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind != StmtKind::Def) continue;
                    auto* d = static_cast<const DefStmt*>(s.get());
                    VType rt = VType::Int;
                    if (d->returnType && d->returnType->kind == ExprKind::NameRef) {
                        const std::string& rtn =
                            static_cast<const NameRefExpr*>(
                                d->returnType.get())->name;
                        if (rtn == "str")        rt = VType::Str;
                        else if (rtn == "bool")  rt = VType::Bool;
                        else if (rtn == "float") rt = VType::Float;
                        else if (rtn == "list")  rt = VType::List;
                        else if (rtn == "map")   rt = VType::Map;
                        else if (rtn == "tuple") rt = VType::Tuple;
                        else if (rtn == "set")   rt = VType::Set;
                    }
                    fnReturnTypes_[d->name] = rt;
                }
                bool hasUserMain = false;
                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Def) {
                        auto* d = static_cast<const DefStmt*>(s.get());
                        if (d->name == "main") hasUserMain = true;
                        emitFn(d);
                    }
                }
                emitMain(program, hasUserMain);
                return out_.str();
            }

        private:
            std::ostringstream                           out_;
            std::unordered_map<std::string, std::string> fnSlots_;
            std::unordered_map<std::string, VType>       fnReturnTypes_;
            std::unordered_map<std::string, VType>       nameTypes_;
            std::vector<std::string>                     breakStack_;
            std::vector<std::string>                     continueStack_;
            int  nextTemp_ = 0;
            int  nextLabel_ = 0;
            bool terminated_ = false;
            VType lastType_ = VType::Int;

            std::string fresh() { return "%t" + std::to_string(nextTemp_++); }
            std::string freshLabel(const std::string& prefix) {
                return "bb_" + prefix + "_" + std::to_string(nextLabel_++);
            }

            void emit(const std::string& s) {
                out_ << "  " << s << "\n";
                size_t sp = s.find(' ');
                std::string op = (sp == std::string::npos) ? s : s.substr(0, sp);
                terminated_ = (op == "ret" || op == "jmp" || op == "br");
            }
            void emitLabel(const std::string& l) {
                out_ << l << ":\n";
                terminated_ = false;
            }
            void emitRaw(const std::string& s) { out_ << s << "\n"; }

            std::string declareSlot(const std::string& name, VType t) {
                auto it = fnSlots_.find(name);
                if (it != fnSlots_.end()) {
                    nameTypes_[name] = t;
                    return it->second;
                }
                std::string slot = "%v_" + name;
                fnSlots_[name] = slot;
                nameTypes_[name] = t;
                emit("i64 " + slot + " = alloca i64");
                return slot;
            }

            std::string loadName(const std::string& name) {
                auto it = fnSlots_.find(name);
                if (it == fnSlots_.end())
                    throw std::runtime_error(
                        "VcbLower: undefined name '" + name + "'");
                std::string t = fresh();
                emit("i64 " + t + " = load " + it->second);
                auto tit = nameTypes_.find(name);
                lastType_ = (tit != nameTypes_.end()) ? tit->second : VType::Int;
                return t;
            }

            bool isFloat(VType t) const { return t == VType::Float; }

            std::string emitExpr(const Expr* e) {
                if (!e) throw std::runtime_error("VcbLower: null expression");
                switch (e->kind) {

                case ExprKind::IntLit: {
                    auto* n = static_cast<const IntLitExpr*>(e);
                    std::string t = fresh();
                    emit("i64 " + t + " = const.i64 " + std::to_string(n->value));
                    lastType_ = VType::Int;
                    return t;
                }
                case ExprKind::FloatLit: {
                    auto* n = static_cast<const FloatLitExpr*>(e);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.17g", n->value);
                    std::string t = fresh();
                    emit("f64 " + t + " = const.f64 " + buf);
                    lastType_ = VType::Float;
                    return t;
                }
                case ExprKind::BoolLit: {
                    auto* n = static_cast<const BoolLitExpr*>(e);
                    std::string t = fresh();
                    emit("i64 " + t + " = const.i64 " + (n->value ? "1" : "0"));
                    lastType_ = VType::Bool;
                    return t;
                }
                case ExprKind::NoneLit: {
                    std::string t = fresh();
                    emit("i64 " + t + " = const.i64 0");
                    lastType_ = VType::Int;
                    return t;
                }
                case ExprKind::ListLit: {
                    auto* n = static_cast<const ListLitExpr*>(e);
                    std::string lst = fresh();
                    emit("i64 " + lst + " = call vayu_list_new()");
                    for (auto& el : n->elements) {
                        std::string v = emitExpr(el.get());
                        emit("call vayu_list_push(" + lst + ", " + v + ")");
                    }
                    lastType_ = VType::List;
                    return lst;
                }
                case ExprKind::MapLit: {
                    auto* n = static_cast<const MapLitExpr*>(e);
                    std::string m = fresh();
                    emit("i64 " + m + " = call vayu_map_new()");
                    for (auto& en : n->entries) {
                        std::string k = emitExpr(en.key.get());
                        std::string v = emitExpr(en.value.get());
                        emit("call vayu_map_put(" + m + ", " + k + ", " + v + ")");
                    }
                    lastType_ = VType::Map;
                    return m;
                }
                case ExprKind::TupleLit: {
                    auto* n = static_cast<const TupleLitExpr*>(e);
                    int count = (int)n->elements.size();
                    std::string cnt = fresh();
                    emit("i64 " + cnt + " = const.i64 " +
                        std::to_string(count));
                    std::string t = fresh();
                    emit("i64 " + t + " = call vayu_tuple_new(" + cnt + ")");
                    for (int i = 0; i < count; ++i) {
                        std::string v = emitExpr(n->elements[i].get());
                        emit("call vayu_tuple_set(" + t + ", " +
                            std::to_string(i) + ", " + v + ")");
                    }
                    lastType_ = VType::Tuple;
                    return t;
                }
                case ExprKind::SetLit: {
                    auto* n = static_cast<const SetLitExpr*>(e);
                    std::string s = fresh();
                    emit("i64 " + s + " = call vayu_set_new()");
                    for (auto& el : n->elements) {
                        std::string v = emitExpr(el.get());
                        emit("call vayu_set_add(" + s + ", " + v + ")");
                    }
                    lastType_ = VType::Set;
                    return s;
                }
                case ExprKind::Slice: {
                    auto* n = static_cast<const SliceExpr*>(e);
                    std::string tgt = emitExpr(n->target.get());
                    VType tt = lastType_;
                    if (tt != VType::Str && tt != VType::List &&
                        tt != VType::Tuple)
                        throw std::runtime_error(
                            "VcbLower: slice on this type not yet supported "
                            "at line " + std::to_string(e->loc.line));
                    std::string a;
                    if (n->start) a = emitExpr(n->start.get());
                    else {
                        a = fresh();
                        emit("i64 " + a + " = const.i64 0");
                    }
                    std::string b;
                    if (n->end) b = emitExpr(n->end.get());
                    else {
                        b = fresh();
                        if (tt == VType::Str)
                            emit("i64 " + b + " = call vayu_str_len(" + tgt + ")");
                        else if (tt == VType::List)
                            emit("i64 " + b + " = call vayu_list_len(" + tgt + ")");
                        else
                            emit("i64 " + b + " = call vayu_tuple_len(" + tgt + ")");
                    }
                    std::string t = fresh();
                    if (tt == VType::Str)
                        emit("i64 " + t + " = call vayu_str_slice(" + tgt +
                            ", " + a + ", " + b + ")");
                    else if (tt == VType::List)
                        emit("i64 " + t + " = call vayu_list_slice(" + tgt +
                            ", " + a + ", " + b + ")");
                    else
                        emit("i64 " + t + " = call vayu_tuple_slice(" + tgt +
                            ", " + a + ", " + b + ")");
                    lastType_ = tt;
                    return t;
                }
                case ExprKind::StringLit: {
                    auto* n = static_cast<const StringLitExpr*>(e);
                    std::string esc;
                    for (char c : n->value) {
                        if (c == '\\') esc += "\\\\";
                        else if (c == '"')  esc += "\\\"";
                        else if (c == '\n') esc += "\\n";
                        else if (c == '\t') esc += "\\t";
                        else if (c == '\r') esc += "\\r";
                        else                esc += c;
                    }
                    std::string t = fresh();
                    emit("i64 " + t + " = const.str \"" + esc + "\"");
                    lastType_ = VType::Str;
                    return t;
                }
                case ExprKind::Grouping:
                    return emitExpr(
                        static_cast<const GroupingExpr*>(e)->inner.get());

                case ExprKind::NameRef: {
                    auto* n = static_cast<const NameRefExpr*>(e);
                    return loadName(n->name);
                }

                case ExprKind::Unary: {
                    auto* u = static_cast<const UnaryExpr*>(e);
                    std::string v = emitExpr(u->operand.get());
                    VType operandType = lastType_;
                    switch (u->op) {
                    case UnOp::Pos: return v;
                    case UnOp::Neg: {
                        if (isFloat(operandType)) {
                            std::string t = fresh();
                            emit("f64 " + t + " = fneg " + v);
                            lastType_ = VType::Float;
                            return t;
                        }
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        std::string t = fresh();
                        emit("i64 " + t + " = sub " + z + ", " + v);
                        lastType_ = operandType;
                        return t;
                    }
                    case UnOp::Not: {
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        std::string t = fresh();
                        emit("i64 " + t + " = eq " + v + ", " + z);
                        lastType_ = VType::Bool;
                        return t;
                    }
                    case UnOp::BNot: {
                        if (operandType != VType::Int)
                            throw std::runtime_error(
                                "VcbLower: '~' requires an int operand "
                                "at line " + std::to_string(e->loc.line));
                        std::string neg1 = fresh();
                        emit("i64 " + neg1 + " = const.i64 -1");
                        std::string t = fresh();
                        emit("i64 " + t + " = xor " + v + ", " + neg1);
                        lastType_ = VType::Int;
                        return t;
                    }
                    default:
                        throw std::runtime_error(
                            "VcbLower: unary op not yet supported at line " +
                            std::to_string(e->loc.line));
                    }
                }

                case ExprKind::Binary: {
                    auto* b = static_cast<const BinaryExpr*>(e);

                    if (b->op == BinOp::And) {
                        std::string slotName = "__and_" +
                            std::to_string(nextTemp_++);
                        std::string slot = declareSlot(slotName, VType::Int);
                        std::string l = emitExpr(b->lhs.get());
                        VType lt = lastType_;
                        std::string evalR = freshLabel("and_eval");
                        std::string done = freshLabel("and_done");
                        emit("store " + l + ", " + slot);
                        emit("br " + l + ", " + evalR + ", " + done);
                        emitLabel(evalR);
                        std::string r = emitExpr(b->rhs.get());
                        VType rt = lastType_;
                        emit("store " + r + ", " + slot);
                        emit("jmp " + done);
                        emitLabel(done);
                        std::string t = fresh();
                        emit("i64 " + t + " = load " + slot);
                        lastType_ = (lt == rt) ? lt : rt;
                        return t;
                    }

                    if (b->op == BinOp::Or) {
                        std::string slotName = "__or_" +
                            std::to_string(nextTemp_++);
                        std::string slot = declareSlot(slotName, VType::Int);
                        std::string l = emitExpr(b->lhs.get());
                        VType lt = lastType_;
                        std::string evalR = freshLabel("or_eval");
                        std::string done = freshLabel("or_done");
                        emit("store " + l + ", " + slot);
                        emit("br " + l + ", " + done + ", " + evalR);
                        emitLabel(evalR);
                        std::string r = emitExpr(b->rhs.get());
                        VType rt = lastType_;
                        emit("store " + r + ", " + slot);
                        emit("jmp " + done);
                        emitLabel(done);
                        std::string t = fresh();
                        emit("i64 " + t + " = load " + slot);
                        lastType_ = (lt == rt) ? lt : rt;
                        return t;
                    }

                    if (b->op == BinOp::Pow) {
                        std::string l = emitExpr(b->lhs.get());
                        VType lt = lastType_;
                        std::string r = emitExpr(b->rhs.get());
                        VType rt = lastType_;
                        if (lt != VType::Int || rt != VType::Int)
                            throw std::runtime_error(
                                "VcbLower: '**' only supports int ** int "
                                "at line " + std::to_string(e->loc.line));
                        if (b->rhs->kind == ExprKind::IntLit) {
                            auto* lit = static_cast<const IntLitExpr*>(
                                b->rhs.get());
                            if (lit->value < 0)
                                throw std::runtime_error(
                                    "VcbLower: '**' with a negative "
                                    "exponent is not supported "
                                    "(Python returns a float) at line " +
                                    std::to_string(e->loc.line));
                        }
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_pow_int(" + l +
                            ", " + r + ")");
                        lastType_ = VType::Int;
                        return t;
                    }

                    if (b->op == BinOp::In) {
                        std::string l = emitExpr(b->lhs.get());
                        VType lt = lastType_;
                        std::string r = emitExpr(b->rhs.get());
                        VType rt = lastType_;
                        if (rt == VType::Map && lt == VType::Str) {
                            std::string t = fresh();
                            emit("i64 " + t + " = call vayu_map_has(" + r +
                                ", " + l + ")");
                            lastType_ = VType::Bool;
                            return t;
                        }
                        throw std::runtime_error(
                            "VcbLower: 'in' only supports map<str, _> on "
                            "the right, at line " +
                            std::to_string(e->loc.line));
                    }

                    std::string l = emitExpr(b->lhs.get());
                    VType lt = lastType_;
                    std::string r = emitExpr(b->rhs.get());
                    VType rt = lastType_;

                    if (lt == VType::Str && rt == VType::Str) {
                        if (b->op == BinOp::Add) {
                            std::string t = fresh();
                            emit("i64 " + t +
                                " = call vayu_str_concat(" + l + ", " + r + ")");
                            lastType_ = VType::Str;
                            return t;
                        }
                        if (b->op == BinOp::Eq) {
                            std::string t = fresh();
                            emit("i64 " + t +
                                " = call vayu_str_eq(" + l + ", " + r + ")");
                            lastType_ = VType::Bool;
                            return t;
                        }
                        if (b->op == BinOp::NotEq) {
                            std::string t = fresh();
                            emit("i64 " + t +
                                " = call vayu_str_eq(" + l + ", " + r + ")");
                            std::string z = fresh();
                            emit("i64 " + z + " = const.i64 0");
                            std::string n = fresh();
                            emit("i64 " + n + " = eq " + t + ", " + z);
                            lastType_ = VType::Bool;
                            return n;
                        }
                    }

                    bool floatCtx = isFloat(lt) || isFloat(rt);

                    std::string op;
                    VType resultType = VType::Int;
                    switch (b->op) {
                    case BinOp::Add:
                    case BinOp::Sub:
                    case BinOp::Mul:
                    case BinOp::Div:
                    case BinOp::Mod:
                    case BinOp::FloorDiv:
                        if (floatCtx) {
                            switch (b->op) {
                            case BinOp::Add: op = "fadd"; break;
                            case BinOp::Sub: op = "fsub"; break;
                            case BinOp::Mul: op = "fmul"; break;
                            case BinOp::Div: op = "fdiv"; break;
                            default:
                                throw std::runtime_error(
                                    "VcbLower: float '%' or '//' not yet "
                                    "supported at line " +
                                    std::to_string(e->loc.line));
                            }
                            resultType = VType::Float;
                        }
                        else {
                            switch (b->op) {
                            case BinOp::Add:      op = "add"; break;
                            case BinOp::Sub:      op = "sub"; break;
                            case BinOp::Mul:      op = "mul"; break;
                            case BinOp::Div:      op = "div"; break;
                            case BinOp::Mod:      op = "mod"; break;
                            case BinOp::FloorDiv: op = "div"; break;
                            default: break;
                            }
                            resultType = VType::Int;
                        }
                        break;
                    case BinOp::Eq:
                    case BinOp::NotEq:
                    case BinOp::Lt:
                    case BinOp::LtEq:
                    case BinOp::Gt:
                    case BinOp::GtEq:
                        if (floatCtx) {
                            switch (b->op) {
                            case BinOp::Eq:    op = "fcmp_eq"; break;
                            case BinOp::NotEq: op = "fcmp_ne"; break;
                            case BinOp::Lt:    op = "fcmp_lt"; break;
                            case BinOp::LtEq:  op = "fcmp_le"; break;
                            case BinOp::Gt:    op = "fcmp_gt"; break;
                            case BinOp::GtEq:  op = "fcmp_ge"; break;
                            default: break;
                            }
                        }
                        else {
                            switch (b->op) {
                            case BinOp::Eq:    op = "eq"; break;
                            case BinOp::NotEq: op = "ne"; break;
                            case BinOp::Lt:    op = "lt"; break;
                            case BinOp::LtEq:  op = "le"; break;
                            case BinOp::Gt:    op = "gt"; break;
                            case BinOp::GtEq:  op = "ge"; break;
                            default: break;
                            }
                        }
                        resultType = VType::Bool;
                        break;
                    case BinOp::BAnd:     op = "and"; resultType = VType::Int; break;
                    case BinOp::BOr:      op = "or";  resultType = VType::Int; break;
                    case BinOp::BXor:     op = "xor"; resultType = VType::Int; break;
                    case BinOp::Shl:      op = "shl"; resultType = VType::Int; break;
                    case BinOp::Shr:      op = "shr"; resultType = VType::Int; break;
                    default:
                        throw std::runtime_error(
                            "VcbLower: binary op '" +
                            std::string(binOpName(b->op)) +
                            "' not yet supported at line " +
                            std::to_string(e->loc.line));
                    }
                    std::string t = fresh();
                    emit("i64 " + t + " = " + op + " " + l + ", " + r);
                    lastType_ = resultType;
                    return t;
                }

                case ExprKind::Call: {
                    auto* c = static_cast<const CallExpr*>(e);
                    if (c->callee->kind == ExprKind::NameRef) {
                        const std::string& fn =
                            static_cast<const NameRefExpr*>(
                                c->callee.get())->name;
                        return emitCall(fn, c);
                    }
                    if (c->callee->kind == ExprKind::Attr) {
                        auto* attr =
                            static_cast<const AttrExpr*>(c->callee.get());
                        return emitMethodCall(attr, c);
                    }
                    throw std::runtime_error(
                        "VcbLower: unsupported callee at line " +
                        std::to_string(e->loc.line));
                }

                case ExprKind::Index: {
                    auto* ix = static_cast<const IndexExpr*>(e);
                    std::string tgt = emitExpr(ix->target.get());
                    VType tgtType = lastType_;
                    std::string idx = emitExpr(ix->index.get());
                    std::string t = fresh();
                    if (tgtType == VType::List) {
                        emit("i64 " + t + " = call vayu_list_get(" + tgt +
                            ", " + idx + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (tgtType == VType::Map) {
                        emit("i64 " + t + " = call vayu_map_get(" + tgt +
                            ", " + idx + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (tgtType == VType::Tuple) {
                        emit("i64 " + t + " = call vayu_tuple_get(" + tgt +
                            ", " + idx + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    throw std::runtime_error(
                        "VcbLower: index on non-collection at line " +
                        std::to_string(e->loc.line));
                }

                default:
                    throw std::runtime_error(
                        "VcbLower: expression kind not yet supported at line " +
                        std::to_string(e->loc.line));
                }
            }

            std::string emitCall(const std::string& name, const CallExpr* c) {
                std::vector<std::string> args;
                std::vector<VType>       argTypes;
                for (auto& a : c->args) {
                    if (!a.name.empty())
                        throw std::runtime_error(
                            "VcbLower: keyword arguments not supported");
                    args.push_back(emitExpr(a.value.get()));
                    argTypes.push_back(lastType_);
                }

                if (name == "print") {
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i > 0) emit("call vayu_print_space()");
                        switch (argTypes[i]) {
                        case VType::Bool:
                            emit("call vayu_print_bool(" + args[i] + ")");
                            break;
                        case VType::Str:
                            emit("call vayu_print_str(" + args[i] + ")");
                            break;
                        case VType::Float:
                            emit("call vayu_print_float(" + args[i] + ")");
                            break;
                        case VType::List:
                            emit("call vayu_print_list(" + args[i] + ")");
                            break;
                        case VType::Map:
                            emit("call vayu_print_map(" + args[i] + ")");
                            break;
                        case VType::Tuple:
                            emit("call vayu_print_tuple(" + args[i] + ")");
                            break;
                        case VType::Set:
                            emit("call vayu_print_list(" + args[i] + ")");
                            break;
                        case VType::Int:
                        default:
                            emit("call vayu_print_int(" + args[i] + ")");
                            break;
                        }
                    }
                    emit("call vayu_print_ln()");
                    std::string z = fresh();
                    emit("i64 " + z + " = const.i64 0");
                    lastType_ = VType::Int;
                    return z;
                }
                if (name == "exit") {
                    if (args.size() != 1)
                        throw std::runtime_error(
                            "VcbLower: exit() takes one int");
                    emit("call vayu_exit(" + args[0] + ")");
                    std::string z = fresh();
                    emit("i64 " + z + " = const.i64 0");
                    lastType_ = VType::Int;
                    return z;
                }
                if (name == "len") {
                    if (args.size() != 1)
                        throw std::runtime_error("VcbLower: len() takes one arg");
                    std::string t = fresh();
                    switch (argTypes[0]) {
                    case VType::List:
                        emit("i64 " + t + " = call vayu_list_len(" +
                            args[0] + ")");
                        break;
                    case VType::Map:
                        emit("i64 " + t + " = call vayu_map_len(" +
                            args[0] + ")");
                        break;
                    case VType::Tuple:
                        emit("i64 " + t + " = call vayu_tuple_len(" +
                            args[0] + ")");
                        break;
                    case VType::Set:
                        emit("i64 " + t + " = call vayu_set_len(" +
                            args[0] + ")");
                        break;
                    default:
                        throw std::runtime_error(
                            "VcbLower: len() on non-collection at line " +
                            std::to_string(c->loc.line));
                    }
                    lastType_ = VType::Int;
                    return t;
                }
                if (name == "int") {
                    if (args.size() != 1)
                        throw std::runtime_error("VcbLower: int() takes one arg");
                    if (argTypes[0] == VType::Float) {
                        std::string t = fresh();
                        emit("i64 " + t + " = fptosi " + args[0]);
                        lastType_ = VType::Int;
                        return t;
                    }
                    lastType_ = VType::Int;
                    return args[0];
                }
                if (name == "float") {
                    if (args.size() != 1)
                        throw std::runtime_error("VcbLower: float() takes one arg");
                    if (argTypes[0] == VType::Float) {
                        lastType_ = VType::Float;
                        return args[0];
                    }
                    std::string t = fresh();
                    emit("i64 " + t + " = sitof " + args[0]);
                    lastType_ = VType::Float;
                    return t;
                }
                if (name == "str") {
                    if (args.size() != 1)
                        throw std::runtime_error("VcbLower: str() takes one arg");
                    if (argTypes[0] == VType::Str) {
                        lastType_ = VType::Str;
                        return args[0];
                    }
                    if (argTypes[0] == VType::Bool) {
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_bool_to_str(" +
                            args[0] + ")");
                        lastType_ = VType::Str;
                        return t;
                    }
                    if (argTypes[0] == VType::Int) {
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_int_to_str(" +
                            args[0] + ")");
                        lastType_ = VType::Str;
                        return t;
                    }
                    throw std::runtime_error(
                        "VcbLower: str() for this type is not yet supported "
                        "at line " + std::to_string(c->loc.line));
                }

                auto fnRit = fnReturnTypes_.find(name);
                if (fnRit == fnReturnTypes_.end()) {
                    throw std::runtime_error(
                        "VcbLower: unknown builtin '" + name +
                        "' at line " + std::to_string(c->loc.line));
                }

                std::string argstr;
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i) argstr += ", ";
                    argstr += args[i];
                }
                std::string t = fresh();
                emit("i64 " + t + " = call " + name + "(" + argstr + ")");
                lastType_ = fnRit->second;
                return t;
            }

            std::string emitMethodCall(const AttrExpr* attr, const CallExpr* c) {
                std::string recv = emitExpr(attr->target.get());
                VType recvType = lastType_;
                const std::string& m = attr->name;

                if (recvType == VType::Str) {
                    if (m == "len") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: str.len takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_len(" + recv + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (m == "char_at") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: str.char_at takes 1 argument");
                        std::string i = emitExpr(c->args[0].value.get());
                        std::string one = fresh();
                        emit("i64 " + one + " = const.i64 1");
                        std::string iEnd = fresh();
                        emit("i64 " + iEnd + " = add " + i + ", " + one);
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_slice(" + recv +
                            ", " + i + ", " + iEnd + ")");
                        lastType_ = VType::Str;
                        return t;
                    }
                    if (m == "to_int") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: str.to_int takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_to_int(" +
                            recv + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (m == "strip") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: str.strip takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_strip(" + recv + ")");
                        lastType_ = VType::Str;
                        return t;
                    }
                    // Phase 31.4.
                    if (m == "split") {
                        if (c->args.size() > 1)
                            throw std::runtime_error(
                                "VcbLower: str.split takes 0 or 1 argument");
                        std::string sep;
                        if (c->args.empty()) {
                            sep = fresh();
                            emit("i64 " + sep + " = const.str \" \"");
                        }
                        else {
                            sep = emitExpr(c->args[0].value.get());
                            if (lastType_ != VType::Str)
                                throw std::runtime_error(
                                    "VcbLower: str.split separator must be str");
                        }
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_split(" + recv +
                            ", " + sep + ")");
                        lastType_ = VType::List;
                        return t;
                    }
                    if (m == "replace") {
                        if (c->args.size() != 2)
                            throw std::runtime_error(
                                "VcbLower: str.replace takes 2 arguments");
                        std::string old_s = emitExpr(c->args[0].value.get());
                        std::string new_s = emitExpr(c->args[1].value.get());
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_replace(" + recv +
                            ", " + old_s + ", " + new_s + ")");
                        lastType_ = VType::Str;
                        return t;
                    }
                    if (m == "upper" || m == "lower") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: str." + m + " takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_" + m + "(" +
                            recv + ")");
                        lastType_ = VType::Str;
                        return t;
                    }
                    if (m == "starts_with" || m == "ends_with" ||
                        m == "contains" || m == "find") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: str." + m + " takes 1 argument");
                        std::string arg = emitExpr(c->args[0].value.get());
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_str_" + m + "(" + recv +
                            ", " + arg + ")");
                        lastType_ = (m == "find") ? VType::Int : VType::Bool;
                        return t;
                    }
                }

                if (recvType == VType::List) {
                    if (m == "append") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: list.append takes 1 argument");
                        std::string v = emitExpr(c->args[0].value.get());
                        emit("call vayu_list_push(" + recv + ", " + v + ")");
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        lastType_ = VType::Int;
                        return z;
                    }
                    if (m == "len") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: list.len takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_list_len(" + recv + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (m == "pop") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: list.pop takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_list_pop(" + recv + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (m == "contains") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: list.contains takes 1 argument");
                        std::string v = emitExpr(c->args[0].value.get());
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_list_contains(" + recv +
                            ", " + v + ")");
                        lastType_ = VType::Bool;
                        return t;
                    }
                    if (m == "index") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: list.index takes 1 argument");
                        std::string v = emitExpr(c->args[0].value.get());
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_list_index(" + recv +
                            ", " + v + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                }

                if (recvType == VType::Set) {
                    if (m == "add") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: set.add takes 1 argument");
                        std::string v = emitExpr(c->args[0].value.get());
                        emit("call vayu_set_add(" + recv + ", " + v + ")");
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        lastType_ = VType::Int;
                        return z;
                    }
                    if (m == "contains") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: set.contains takes 1 argument");
                        std::string v = emitExpr(c->args[0].value.get());
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_set_contains(" +
                            recv + ", " + v + ")");
                        lastType_ = VType::Bool;
                        return t;
                    }
                    if (m == "len") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: set.len takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_set_len(" +
                            recv + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (m == "remove") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: set.remove takes 1 argument");
                        std::string v = emitExpr(c->args[0].value.get());
                        emit("call vayu_set_remove(" + recv + ", " + v + ")");
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        lastType_ = VType::Int;
                        return z;
                    }
                    if (m == "clear") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: set.clear takes no arguments");
                        emit("call vayu_set_clear(" + recv + ")");
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        lastType_ = VType::Int;
                        return z;
                    }
                    if (m == "copy") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: set.copy takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_set_copy(" + recv + ")");
                        lastType_ = VType::Set;
                        return t;
                    }
                }

                if (recvType == VType::Map) {
                    if (m == "put" || m == "set") {
                        if (c->args.size() != 2)
                            throw std::runtime_error(
                                "VcbLower: map.put takes 2 arguments");
                        std::string k = emitExpr(c->args[0].value.get());
                        std::string v = emitExpr(c->args[1].value.get());
                        emit("call vayu_map_put(" + recv + ", " + k + ", " +
                            v + ")");
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        lastType_ = VType::Int;
                        return z;
                    }
                    if (m == "get") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: map.get takes 1 argument");
                        std::string k = emitExpr(c->args[0].value.get());
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_map_get(" + recv +
                            ", " + k + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                    if (m == "has" || m == "contains") {
                        if (c->args.size() != 1)
                            throw std::runtime_error(
                                "VcbLower: map.has takes 1 argument");
                        std::string k = emitExpr(c->args[0].value.get());
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_map_has(" + recv +
                            ", " + k + ")");
                        lastType_ = VType::Bool;
                        return t;
                    }
                    if (m == "len") {
                        if (!c->args.empty())
                            throw std::runtime_error(
                                "VcbLower: map.len takes no arguments");
                        std::string t = fresh();
                        emit("i64 " + t + " = call vayu_map_len(" + recv + ")");
                        lastType_ = VType::Int;
                        return t;
                    }
                }

                throw std::runtime_error(
                    "VcbLower: unsupported method '" + m +
                    "' at line " + std::to_string(c->loc.line));
            }

            void emitStmt(const Stmt* s) {
                if (!s) return;
                switch (s->kind) {

                case StmtKind::Expr: {
                    auto* n = static_cast<const ExprStmt*>(s);
                    emitExpr(n->expr.get());
                    return;
                }
                case StmtKind::Return: {
                    auto* n = static_cast<const ReturnStmt*>(s);
                    if (n->value) {
                        std::string v = emitExpr(n->value.get());
                        emit("ret " + v);
                    }
                    else {
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        emit("ret " + z);
                    }
                    return;
                }
                case StmtKind::AnnotAssign: {
                    auto* n = static_cast<const AnnotAssignStmt*>(s);
                    if (!n->value)
                        throw std::runtime_error(
                            "VcbLower: uninitialized annotation at line " +
                            std::to_string(s->loc.line));
                    std::string v = emitExpr(n->value.get());
                    std::string slot = declareSlot(n->name, lastType_);
                    emit("store " + v + ", " + slot);
                    return;
                }
                case StmtKind::Assign: {
                    auto* n = static_cast<const AssignStmt*>(s);

                    if (n->target->kind == ExprKind::Index) {
                        auto* ix = static_cast<const IndexExpr*>(
                            n->target.get());
                        std::string base = emitExpr(ix->target.get());
                        VType baseType = lastType_;
                        std::string idx = emitExpr(ix->index.get());
                        std::string val = emitExpr(n->value.get());
                        if (baseType == VType::List) {
                            emit("call vayu_list_set(" + base + ", " +
                                idx + ", " + val + ")");
                            return;
                        }
                        if (baseType == VType::Map) {
                            emit("call vayu_map_put(" + base + ", " +
                                idx + ", " + val + ")");
                            return;
                        }
                        throw std::runtime_error(
                            "VcbLower: index-assign on non-collection "
                            "at line " + std::to_string(s->loc.line));
                    }

                    if (n->target->kind != ExprKind::NameRef)
                        throw std::runtime_error(
                            "VcbLower: only NameRef and Index assignment "
                            "targets supported at line " +
                            std::to_string(s->loc.line));
                    const std::string& nm =
                        static_cast<const NameRefExpr*>(n->target.get())->name;
                    std::string v = emitExpr(n->value.get());
                    std::string slot = declareSlot(nm, lastType_);
                    emit("store " + v + ", " + slot);
                    return;
                }
                case StmtKind::If: {
                    emitIf(static_cast<const IfStmt*>(s));
                    return;
                }
                case StmtKind::While: {
                    emitWhile(static_cast<const WhileStmt*>(s));
                    return;
                }
                case StmtKind::For: {
                    emitFor(static_cast<const ForStmt*>(s));
                    return;
                }
                case StmtKind::Break: {
                    if (breakStack_.empty())
                        throw std::runtime_error(
                            "VcbLower: 'break' outside loop at line " +
                            std::to_string(s->loc.line));
                    emit("jmp " + breakStack_.back());
                    return;
                }
                case StmtKind::Continue: {
                    if (continueStack_.empty())
                        throw std::runtime_error(
                            "VcbLower: 'continue' outside loop at line " +
                            std::to_string(s->loc.line));
                    emit("jmp " + continueStack_.back());
                    return;
                }
                case StmtKind::Pass:
                    return;
                default:
                    throw std::runtime_error(
                        "VcbLower: statement kind not yet supported at line " +
                        std::to_string(s->loc.line));
                }
            }

            void emitIf(const IfStmt* n) {
                std::string merge = freshLabel("if_merge");
                struct Branch { const Expr* cond; const Block* body; };
                std::vector<Branch> branches;
                branches.push_back({ n->cond.get(), &n->thenBody });
                for (auto& ec : n->elifs)
                    branches.push_back({ ec.cond.get(), &ec.body });
                const Block* elseB = n->elseBody ? &*n->elseBody : nullptr;

                for (size_t i = 0; i < branches.size(); ++i) {
                    bool isLast = (i + 1 == branches.size());
                    std::string thenL = freshLabel("if_then");
                    std::string nextL = (!isLast || elseB)
                        ? freshLabel("if_next") : merge;
                    std::string condReg = emitExpr(branches[i].cond);
                    emit("br " + condReg + ", " + thenL + ", " + nextL);
                    emitLabel(thenL);
                    emitBlock(*branches[i].body);
                    if (!terminated_) emit("jmp " + merge);
                    if (nextL != merge) emitLabel(nextL);
                }
                if (elseB) {
                    emitBlock(*elseB);
                    if (!terminated_) emit("jmp " + merge);
                }
                emitLabel(merge);
            }

            void emitWhile(const WhileStmt* n) {
                std::string topL = freshLabel("while_top");
                std::string bodyL = freshLabel("while_body");
                std::string exitL = freshLabel("while_exit");
                emit("jmp " + topL);
                emitLabel(topL);
                std::string cond = emitExpr(n->cond.get());
                emit("br " + cond + ", " + bodyL + ", " + exitL);
                emitLabel(bodyL);
                breakStack_.push_back(exitL);
                continueStack_.push_back(topL);
                emitBlock(n->body);
                continueStack_.pop_back();
                breakStack_.pop_back();
                if (!terminated_) emit("jmp " + topL);
                emitLabel(exitL);
            }

            void emitFor(const ForStmt* n) {
                bool isRangeCall = false;
                if (n->iterable->kind == ExprKind::Call) {
                    auto* c = static_cast<const CallExpr*>(
                        n->iterable.get());
                    if (c->callee->kind == ExprKind::NameRef &&
                        static_cast<const NameRefExpr*>(
                            c->callee.get())->name == "range")
                        isRangeCall = true;
                }
                if (!isRangeCall) {
                    std::string listExpr = emitExpr(n->iterable.get());
                    VType it = lastType_;
                    if (it != VType::List && it != VType::Set)
                        throw std::runtime_error(
                            "VcbLower: for-in over this type is not yet "
                            "supported at line " +
                            std::to_string(n->loc.line));

                    std::string count = fresh();
                    if (it == VType::List)
                        emit("i64 " + count + " = call vayu_list_len(" +
                            listExpr + ")");
                    else
                        emit("i64 " + count + " = call vayu_set_len(" +
                            listExpr + ")");

                    std::string idxName = "__foridx_" +
                        std::to_string(nextTemp_++);
                    std::string idxSlot = declareSlot(idxName, VType::Int);
                    std::string zero = fresh();
                    emit("i64 " + zero + " = const.i64 0");
                    emit("store " + zero + ", " + idxSlot);

                    std::string topL = freshLabel("fl_top");
                    std::string bodyL = freshLabel("fl_body");
                    std::string stepL = freshLabel("fl_step");
                    std::string exitL = freshLabel("fl_exit");

                    emitLabel(topL);
                    std::string i = fresh();
                    emit("i64 " + i + " = load " + idxSlot);
                    std::string cmp = fresh();
                    emit("i64 " + cmp + " = lt " + i + ", " + count);
                    emit("br " + cmp + ", " + bodyL + ", " + exitL);

                    emitLabel(bodyL);
                    std::string elem = fresh();
                    if (it == VType::List)
                        emit("i64 " + elem + " = call vayu_list_get(" +
                            listExpr + ", " + i + ")");
                    else
                        emit("i64 " + elem + " = call vayu_list_get(" +
                            listExpr + ", " + i + ")");
                    std::string tSlot = declareSlot(n->targetName, VType::Int);
                    emit("store " + elem + ", " + tSlot);

                    breakStack_.push_back(exitL);
                    continueStack_.push_back(stepL);
                    emitBlock(n->body);
                    continueStack_.pop_back();
                    breakStack_.pop_back();
                    if (!terminated_) emit("jmp " + stepL);

                    emitLabel(stepL);
                    std::string i2 = fresh();
                    emit("i64 " + i2 + " = load " + idxSlot);
                    std::string one = fresh();
                    emit("i64 " + one + " = const.i64 1");
                    std::string iN = fresh();
                    emit("i64 " + iN + " = add " + i2 + ", " + one);
                    emit("store " + iN + ", " + idxSlot);
                    emit("jmp " + topL);

                    emitLabel(exitL);
                    return;
                }
                if (n->iterable->kind != ExprKind::Call)
                    throw std::runtime_error(
                        "VcbLower: `for` requires `range(...)` at line " +
                        std::to_string(n->loc.line));
                auto* call = static_cast<const CallExpr*>(n->iterable.get());
                if (call->callee->kind != ExprKind::NameRef ||
                    static_cast<const NameRefExpr*>(call->callee.get())->name
                    != "range")
                    throw std::runtime_error(
                        "VcbLower: `for` requires `range(...)` at line " +
                        std::to_string(n->loc.line));
                if (call->args.empty() || call->args.size() > 3)
                    throw std::runtime_error(
                        "VcbLower: range() takes 1-3 arguments at line " +
                        std::to_string(n->loc.line));

                std::vector<std::string> argVals;
                for (auto& a : call->args) {
                    if (!a.name.empty())
                        throw std::runtime_error(
                            "VcbLower: keyword args to range() not supported");
                    argVals.push_back(emitExpr(a.value.get()));
                }
                std::string startReg, stopReg, stepReg;
                bool stepPositive = true;
                if (argVals.size() == 1) {
                    std::string z = fresh();
                    emit("i64 " + z + " = const.i64 0");
                    startReg = z;
                    stopReg = argVals[0];
                    std::string one = fresh();
                    emit("i64 " + one + " = const.i64 1");
                    stepReg = one;
                }
                else if (argVals.size() == 2) {
                    startReg = argVals[0];
                    stopReg = argVals[1];
                    std::string one = fresh();
                    emit("i64 " + one + " = const.i64 1");
                    stepReg = one;
                }
                else {
                    if (call->args[2].value->kind != ExprKind::IntLit)
                        throw std::runtime_error(
                            "VcbLower: range() step must be an int "
                            "literal at line " +
                            std::to_string(n->loc.line));
                    auto* stepLit = static_cast<const IntLitExpr*>(
                        call->args[2].value.get());
                    if (stepLit->value == 0)
                        throw std::runtime_error(
                            "VcbLower: range() step must be non-zero "
                            "at line " + std::to_string(n->loc.line));
                    startReg = argVals[0];
                    stopReg = argVals[1];
                    stepReg = argVals[2];
                    stepPositive = (stepLit->value > 0);
                }

                std::string slot = declareSlot(n->targetName, VType::Int);
                emit("store " + startReg + ", " + slot);

                std::string topL = freshLabel("for_top");
                std::string bodyL = freshLabel("for_body");
                std::string stepL = freshLabel("for_step");
                std::string exitL = freshLabel("for_exit");

                emitLabel(topL);
                std::string iCur = fresh();
                emit("i64 " + iCur + " = load " + slot);
                std::string cmp = fresh();
                if (stepPositive)
                    emit("i64 " + cmp + " = lt " + iCur + ", " + stopReg);
                else
                    emit("i64 " + cmp + " = gt " + iCur + ", " + stopReg);
                emit("br " + cmp + ", " + bodyL + ", " + exitL);

                emitLabel(bodyL);
                breakStack_.push_back(exitL);
                continueStack_.push_back(stepL);
                emitBlock(n->body);
                continueStack_.pop_back();
                breakStack_.pop_back();
                if (!terminated_) emit("jmp " + stepL);

                emitLabel(stepL);
                std::string iCur2 = fresh();
                emit("i64 " + iCur2 + " = load " + slot);
                std::string iNxt = fresh();
                emit("i64 " + iNxt + " = add " + iCur2 + ", " + stepReg);
                emit("store " + iNxt + ", " + slot);
                emit("jmp " + topL);
                emitLabel(exitL);
            }

            void emitBlock(const Block& b) {
                for (auto& s : b.stmts) {
                    emitStmt(s.get());
                    if (terminated_) break;
                }
            }

            void emitFn(const DefStmt* d) {
                fnSlots_.clear();
                nameTypes_.clear();
                breakStack_.clear();
                continueStack_.clear();
                nextTemp_ = 0; nextLabel_ = 0;
                terminated_ = false; lastType_ = VType::Int;

                std::string emittedName =
                    (d->name == "main") ? "__vayu_user_main" : d->name;
                std::ostringstream hdr;
                hdr << "func " << emittedName << "(";
                for (size_t i = 0; i < d->params.size(); ++i) {
                    if (i) hdr << ", ";
                    hdr << "%p" << i << ": i64";
                }
                hdr << ") -> i64 {\n";
                emitRaw(hdr.str());
                emitLabel("entry");

                for (size_t i = 0; i < d->params.size(); ++i) {
                    VType pt = VType::Int;
                    if (d->params[i].type &&
                        d->params[i].type->kind == ExprKind::NameRef) {
                        const std::string& tn =
                            static_cast<const NameRefExpr*>(
                                d->params[i].type.get())->name;
                        if (tn == "str")        pt = VType::Str;
                        else if (tn == "bool")  pt = VType::Bool;
                        else if (tn == "float") pt = VType::Float;
                        else if (tn == "list")  pt = VType::List;
                        else if (tn == "map")   pt = VType::Map;
                        else if (tn == "tuple") pt = VType::Tuple;
                        else if (tn == "set")   pt = VType::Set;
                    }
                    std::string slot = declareSlot(d->params[i].name, pt);
                    emit("store %p" + std::to_string(i) + ", " + slot);
                }
                emitBlock(d->body);
                if (!terminated_) {
                    std::string z = fresh();
                    emit("i64 " + z + " = const.i64 0");
                    emit("ret " + z);
                }
                emitRaw("}\n");
            }

            void emitMain(const Block& program, bool hasUserMain) {
                fnSlots_.clear();
                nameTypes_.clear();
                breakStack_.clear();
                continueStack_.clear();
                nextTemp_ = 0; nextLabel_ = 0;
                terminated_ = false; lastType_ = VType::Int;

                emitRaw("func main() -> i64 {");
                emitLabel("entry");
                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Def) continue;
                    emitStmt(s.get());
                    if (terminated_) break;
                }
                if (!terminated_ && hasUserMain) {
                    std::string t = fresh();
                    emit("i64 " + t + " = call __vayu_user_main()");
                    emit("ret " + t);
                    terminated_ = true;
                }
                if (!terminated_) {
                    std::string z = fresh();
                    emit("i64 " + z + " = const.i64 0");
                    emit("ret " + z);
                }
                emitRaw("}\n");
            }
        };

    } // namespace

    std::string VcbLower::lower(const Block& program, const std::string& /*sourceDir*/) {
        Lower l;
        return l.run(program);
    }

} // namespace vayu