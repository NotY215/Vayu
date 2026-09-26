#include "VcbLower.hpp"
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace vayu {

    namespace {

        class Lower {
        public:
            std::string run(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Def) {
                        auto* d = static_cast<const DefStmt*>(s.get());
                        if (d->name == "main")
                            throw std::runtime_error(
                                "VcbLower: user-defined 'main' is not supported; "
                                "top-level statements form the entry point");
                        emitFn(d);
                    }
                }
                emitMain(program);
                return out_.str();
            }

        private:
            std::ostringstream                           out_;
            std::unordered_map<std::string, std::string> fnSlots_;
            int  nextTemp_ = 0;
            int  nextLabel_ = 0;
            bool terminated_ = false;

            std::string fresh() { return "%t" + std::to_string(nextTemp_++); }

            void emit(const std::string& s) {
                out_ << "  " << s << "\n";
                terminated_ = (s.rfind("ret", 0) == 0);
            }
            void emitLabel(const std::string& l) {
                out_ << l << ":\n";
                terminated_ = false;
            }
            void emitRaw(const std::string& s) { out_ << s << "\n"; }

            std::string declareSlot(const std::string& name) {
                auto it = fnSlots_.find(name);
                if (it != fnSlots_.end()) return it->second;
                std::string slot = "%v_" + name;
                fnSlots_[name] = slot;
                emit("i64 " + slot + " = alloca i64");
                std::string z = fresh();
                emit("i64 " + z + " = const.i64 0");
                emit("store " + z + ", " + slot);
                return slot;
            }

            std::string loadName(const std::string& name) {
                auto it = fnSlots_.find(name);
                if (it == fnSlots_.end())
                    throw std::runtime_error(
                        "VcbLower: undefined name '" + name + "'");
                std::string t = fresh();
                emit("i64 " + t + " = load " + it->second);
                return t;
            }

            std::string emitExpr(const Expr* e) {
                if (!e) throw std::runtime_error("VcbLower: null expression");
                switch (e->kind) {

                case ExprKind::IntLit: {
                    auto* n = static_cast<const IntLitExpr*>(e);
                    std::string t = fresh();
                    emit("i64 " + t + " = const.i64 " + std::to_string(n->value));
                    return t;
                }
                case ExprKind::BoolLit: {
                    auto* n = static_cast<const BoolLitExpr*>(e);
                    std::string t = fresh();
                    emit("i64 " + t + " = const.i64 " + (n->value ? "1" : "0"));
                    return t;
                }
                case ExprKind::NoneLit: {
                    std::string t = fresh();
                    emit("i64 " + t + " = const.i64 0");
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
                    switch (u->op) {
                    case UnOp::Pos: return v;
                    case UnOp::Neg: {
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        std::string t = fresh();
                        emit("i64 " + t + " = sub " + z + ", " + v);
                        return t;
                    }
                    case UnOp::Not: {
                        std::string z = fresh();
                        emit("i64 " + z + " = const.i64 0");
                        std::string t = fresh();
                        emit("i64 " + t + " = eq " + v + ", " + z);
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
                    std::string l = emitExpr(b->lhs.get());
                    std::string r = emitExpr(b->rhs.get());
                    std::string op;
                    switch (b->op) {
                    case BinOp::Add:      op = "add"; break;
                    case BinOp::Sub:      op = "sub"; break;
                    case BinOp::Mul:      op = "mul"; break;
                    case BinOp::Div:      op = "div"; break;
                    case BinOp::FloorDiv: op = "div"; break;
                    case BinOp::Mod:      op = "mod"; break;
                    case BinOp::Eq:       op = "eq";  break;
                    case BinOp::NotEq:    op = "ne";  break;
                    case BinOp::Lt:       op = "lt";  break;
                    case BinOp::LtEq:     op = "le";  break;
                    case BinOp::Gt:       op = "gt";  break;
                    case BinOp::GtEq:     op = "ge";  break;
                    case BinOp::BAnd:     op = "and"; break;
                    case BinOp::BOr:      op = "or";  break;
                    case BinOp::BXor:     op = "xor"; break;
                    case BinOp::Shl:      op = "shl"; break;
                    case BinOp::Shr:      op = "shr"; break;
                    default:
                        throw std::runtime_error(
                            "VcbLower: binary op '" +
                            std::string(binOpName(b->op)) +
                            "' not yet supported at line " +
                            std::to_string(e->loc.line));
                    }
                    std::string t = fresh();
                    emit("i64 " + t + " = " + op + " " + l + ", " + r);
                    return t;
                }

                case ExprKind::Call: {
                    auto* c = static_cast<const CallExpr*>(e);
                    if (c->callee->kind != ExprKind::NameRef)
                        throw std::runtime_error(
                            "VcbLower: only NameRef callees supported at line " +
                            std::to_string(e->loc.line));
                    const std::string& fn =
                        static_cast<const NameRefExpr*>(c->callee.get())->name;
                    return emitCall(fn, c);
                }

                default:
                    throw std::runtime_error(
                        "VcbLower: expression kind not yet supported at line " +
                        std::to_string(e->loc.line));
                }
            }

            std::string emitCall(const std::string& name, const CallExpr* c) {
                std::vector<std::string> args;
                for (auto& a : c->args) {
                    if (!a.name.empty())
                        throw std::runtime_error(
                            "VcbLower: keyword arguments not supported");
                    args.push_back(emitExpr(a.value.get()));
                }

                if (name == "print") {
                    for (auto& a : args)
                        emit("call vayu_print_int(" + a + ")");
                    emit("call vayu_print_ln()");
                    std::string z = fresh();
                    emit("i64 " + z + " = const.i64 0");
                    return z;
                }
                if (name == "exit") {
                    if (args.size() != 1)
                        throw std::runtime_error("VcbLower: exit() takes one int");
                    emit("call vayu_exit(" + args[0] + ")");
                    std::string z = fresh();
                    emit("i64 " + z + " = const.i64 0");
                    return z;
                }

                std::string argstr;
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i) argstr += ", ";
                    argstr += args[i];
                }
                std::string t = fresh();
                emit("i64 " + t + " = call " + name + "(" + argstr + ")");
                return t;
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
                    std::string slot = declareSlot(n->name);
                    emit("store " + v + ", " + slot);
                    return;
                }

                case StmtKind::Assign: {
                    auto* n = static_cast<const AssignStmt*>(s);
                    if (n->target->kind != ExprKind::NameRef)
                        throw std::runtime_error(
                            "VcbLower: only NameRef assignment targets supported "
                            "at line " + std::to_string(s->loc.line));
                    const std::string& nm =
                        static_cast<const NameRefExpr*>(n->target.get())->name;
                    std::string v = emitExpr(n->value.get());
                    std::string slot = declareSlot(nm);
                    emit("store " + v + ", " + slot);
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

            void emitBlock(const Block& b) {
                for (auto& s : b.stmts) {
                    emitStmt(s.get());
                    if (terminated_) break;
                }
            }

            void emitFn(const DefStmt* d) {
                fnSlots_.clear();
                nextTemp_ = 0;
                nextLabel_ = 0;
                terminated_ = false;

                std::ostringstream hdr;
                hdr << "func " << d->name << "(";
                for (size_t i = 0; i < d->params.size(); ++i) {
                    if (i) hdr << ", ";
                    hdr << "%p" << i << ": i64";
                }
                hdr << ") -> i64 {\n";
                emitRaw(hdr.str());
                emitLabel("entry");

                for (size_t i = 0; i < d->params.size(); ++i) {
                    std::string slot = declareSlot(d->params[i].name);
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

            void emitMain(const Block& program) {
                fnSlots_.clear();
                nextTemp_ = 0;
                nextLabel_ = 0;
                terminated_ = false;

                emitRaw("func main() -> i64 {");
                emitLabel("entry");

                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Def) continue;
                    emitStmt(s.get());
                    if (terminated_) break;
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