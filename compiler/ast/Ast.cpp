#include "Ast.hpp"
#include <cstdio>
#include <string>

namespace vayu {

    const char* binOpName(BinOp op) {
        switch (op) {
        case BinOp::Add: return "+"; case BinOp::Sub: return "-";
        case BinOp::Mul: return "*"; case BinOp::Div: return "/";
        case BinOp::FloorDiv: return "//"; case BinOp::Mod: return "%";
        case BinOp::Pow: return "**"; case BinOp::Eq: return "==";
        case BinOp::NotEq: return "!="; case BinOp::Lt: return "<";
        case BinOp::Gt: return ">"; case BinOp::LtEq: return "<=";
        case BinOp::GtEq: return ">="; case BinOp::And: return "and";
        case BinOp::Or: return "or"; case BinOp::In: return "in";
        case BinOp::Is: return "is";
        case BinOp::BAnd: return "&"; case BinOp::BOr: return "|";
        case BinOp::BXor: return "^"; case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        }
        return "?";
    }
    const char* unOpName(UnOp op) {
        switch (op) {
        case UnOp::Neg: return "-";
        case UnOp::Pos: return "+";
        case UnOp::Not: return "not";
        case UnOp::BNot: return "~";
        }
        return "?";
    }

    namespace {
        constexpr const char* TEE = "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ";
        constexpr const char* ELBOW = "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ";
        constexpr const char* PIPE = "\xE2\x94\x82   ";
        constexpr const char* BLANK = "    ";
        void putLabel(const std::string& p, bool last, const std::string& l) {
            std::printf("%s%s%s\n", p.c_str(), last ? ELBOW : TEE, l.c_str());
        }
        std::string childPrefix(const std::string& p, bool last) {
            return p + (last ? BLANK : PIPE);
        }
    }

    static void prExpr(const Expr* e, const std::string& p, bool last);
    static void prStmt(const Stmt* s, const std::string& p, bool last);
    static void prBlock(const Block& b, const std::string& p);

    static void prExpr(const Expr* e, const std::string& prefix, bool isLast) {
        if (!e) return;
        switch (e->kind) {
        case ExprKind::IntLit: {
            auto* n = static_cast<const IntLitExpr*>(e);
            putLabel(prefix, isLast, "Int(" + n->text + ")"); break;
        }
        case ExprKind::FloatLit: {
            auto* n = static_cast<const FloatLitExpr*>(e);
            putLabel(prefix, isLast, "Float(" + n->text + ")"); break;
        }
        case ExprKind::StringLit: {
            auto* n = static_cast<const StringLitExpr*>(e);
            putLabel(prefix, isLast, "String(\"" + n->value + "\")"); break;
        }
        case ExprKind::CharLit: {
            auto* n = static_cast<const CharLitExpr*>(e);
            putLabel(prefix, isLast, "Char('" + n->value + "')"); break;
        }
        case ExprKind::BoolLit: {
            auto* n = static_cast<const BoolLitExpr*>(e);
            putLabel(prefix, isLast, n->value ? "Bool(true)" : "Bool(false)"); break;
        }
        case ExprKind::NoneLit: putLabel(prefix, isLast, "None"); break;
        case ExprKind::NameRef: {
            auto* n = static_cast<const NameRefExpr*>(e);
            putLabel(prefix, isLast, "Name(" + n->name + ")"); break;
        }
        case ExprKind::Unary: {
            auto* n = static_cast<const UnaryExpr*>(e);
            putLabel(prefix, isLast, std::string("Unary(") + unOpName(n->op) + ")");
            prExpr(n->operand.get(), childPrefix(prefix, isLast), true); break;
        }
        case ExprKind::Binary: {
            auto* n = static_cast<const BinaryExpr*>(e);
            putLabel(prefix, isLast, std::string("Binary(") + binOpName(n->op) + ")");
            std::string cp = childPrefix(prefix, isLast);
            prExpr(n->lhs.get(), cp, false); prExpr(n->rhs.get(), cp, true); break;
        }
        case ExprKind::Grouping: {
            auto* n = static_cast<const GroupingExpr*>(e);
            putLabel(prefix, isLast, "Group");
            prExpr(n->inner.get(), childPrefix(prefix, isLast), true); break;
        }
        case ExprKind::Call: {
            auto* n = static_cast<const CallExpr*>(e);
            putLabel(prefix, isLast, "Call");
            std::string cp = childPrefix(prefix, isLast);
            bool hasArgs = !n->args.empty();
            putLabel(cp, !hasArgs, "callee");
            prExpr(n->callee.get(), childPrefix(cp, !hasArgs), true);
            if (hasArgs) {
                putLabel(cp, true, "args");
                std::string ap = childPrefix(cp, true);
                for (size_t i = 0; i < n->args.size(); ++i) {
                    const auto& a = n->args[i];
                    bool last = (i + 1 == n->args.size());
                    if (!a.name.empty()) {
                        putLabel(ap, last, "kw:" + a.name);
                        prExpr(a.value.get(), childPrefix(ap, last), true);
                    }
                    else prExpr(a.value.get(), ap, last);
                }
            } break;
        }
        case ExprKind::Attr: {
            auto* n = static_cast<const AttrExpr*>(e);
            putLabel(prefix, isLast, "Attr(." + n->name + ")");
            prExpr(n->target.get(), childPrefix(prefix, isLast), true); break;
        }
        case ExprKind::Index: {
            auto* n = static_cast<const IndexExpr*>(e);
            putLabel(prefix, isLast, "Index");
            std::string cp = childPrefix(prefix, isLast);
            prExpr(n->target.get(), cp, false);
            prExpr(n->index.get(), cp, true); break;
        }
        case ExprKind::ListLit: {
            auto* n = static_cast<const ListLitExpr*>(e);
            putLabel(prefix, isLast, "ListLit[" + std::to_string(n->elements.size()) + "]");
            std::string cp = childPrefix(prefix, isLast);
            for (size_t i = 0; i < n->elements.size(); ++i)
                prExpr(n->elements[i].get(), cp, i + 1 == n->elements.size());
            break;
        }
        case ExprKind::MapLit: {
            auto* n = static_cast<const MapLitExpr*>(e);
            putLabel(prefix, isLast, "MapLit{" + std::to_string(n->entries.size()) + "}");
            std::string cp = childPrefix(prefix, isLast);
            for (size_t i = 0; i < n->entries.size(); ++i) {
                bool last = (i + 1 == n->entries.size());
                putLabel(cp, last, "entry");
                std::string ep = childPrefix(cp, last);
                putLabel(ep, false, "key");
                prExpr(n->entries[i].key.get(), childPrefix(ep, false), true);
                putLabel(ep, true, "value");
                prExpr(n->entries[i].value.get(), childPrefix(ep, true), true);
            } break;
        }
        case ExprKind::Lambda: {
            auto* n = static_cast<const LambdaExpr*>(e);
            std::string hdr = "Lambda(";
            for (size_t i = 0; i < n->params.size(); ++i) {
                if (i) hdr += ", ";
                hdr += n->params[i];
            }
            hdr += ")";
            putLabel(prefix, isLast, hdr);
            prExpr(n->body.get(), childPrefix(prefix, isLast), true);
            break;
        }
        case ExprKind::GenericType: {
            auto* n = static_cast<const GenericTypeExpr*>(e);
            std::string s = "GenericType(" + n->name + "<";
            for (size_t i = 0; i < n->typeArgs.size(); ++i) {
                if (i) s += ", ";
                if (n->typeArgs[i]->kind == ExprKind::NameRef)
                    s += static_cast<const NameRefExpr*>(n->typeArgs[i].get())->name;
                else if (n->typeArgs[i]->kind == ExprKind::GenericType) s += "...";
                else s += "?";
            }
            s += ">)";
            putLabel(prefix, isLast, s);
            break;
        }
        }
    }

    static void prBlock(const Block& b, const std::string& p) {
        if (b.stmts.empty()) return;
        for (size_t i = 0; i < b.stmts.size(); ++i)
            prStmt(b.stmts[i].get(), p, i + 1 == b.stmts.size());
    }

    static void prStmt(const Stmt* s, const std::string& prefix, bool isLast) {
        if (!s) return;
        switch (s->kind) {
        case StmtKind::Expr: {
            auto* n = static_cast<const ExprStmt*>(s);
            putLabel(prefix, isLast, "ExprStmt");
            prExpr(n->expr.get(), childPrefix(prefix, isLast), true); break;
        }
        case StmtKind::Assign: {
            auto* n = static_cast<const AssignStmt*>(s);
            putLabel(prefix, isLast, "Assign");
            std::string cp = childPrefix(prefix, isLast);
            putLabel(cp, false, "target");
            prExpr(n->target.get(), childPrefix(cp, false), true);
            putLabel(cp, true, "value");
            prExpr(n->value.get(), childPrefix(cp, true), true); break;
        }
        case StmtKind::AnnotAssign: {
            auto* n = static_cast<const AnnotAssignStmt*>(s);
            std::string lbl = "AnnotAssign(" + n->name + ")";
            putLabel(prefix, isLast, lbl);
            std::string cp = childPrefix(prefix, isLast);
            putLabel(cp, n->value ? false : true, "type");
            prExpr(n->type.get(), childPrefix(cp, n->value ? false : true), true);
            if (n->value) {
                putLabel(cp, true, "value");
                prExpr(n->value.get(), childPrefix(cp, true), true);
            }
            break;
        }
        case StmtKind::If: {
            auto* n = static_cast<const IfStmt*>(s);
            putLabel(prefix, isLast, "If");
            std::string cp = childPrefix(prefix, isLast);
            size_t total = 2 + n->elifs.size() + (n->elseBody ? 1 : 0), idx = 0;
            putLabel(cp, false, "cond");
            prExpr(n->cond.get(), childPrefix(cp, false), true); ++idx;
            bool tL = (idx + 1 == total);
            putLabel(cp, tL, "then");
            prBlock(n->thenBody, childPrefix(cp, tL)); ++idx;
            for (auto& ec : n->elifs) {
                bool eL = (idx + 1 == total);
                putLabel(cp, eL, "elif");
                std::string ep = childPrefix(cp, eL);
                putLabel(ep, false, "cond");
                prExpr(ec.cond.get(), childPrefix(ep, false), true);
                putLabel(ep, true, "then");
                prBlock(ec.body, childPrefix(ep, true)); ++idx;
            }
            if (n->elseBody) {
                putLabel(cp, true, "else");
                prBlock(*n->elseBody, childPrefix(cp, true));
            } break;
        }
        case StmtKind::While: {
            auto* n = static_cast<const WhileStmt*>(s);
            putLabel(prefix, isLast, "While");
            std::string cp = childPrefix(prefix, isLast);
            putLabel(cp, false, "cond");
            prExpr(n->cond.get(), childPrefix(cp, false), true);
            putLabel(cp, true, "body");
            prBlock(n->body, childPrefix(cp, true)); break;
        }
        case StmtKind::For: {
            auto* n = static_cast<const ForStmt*>(s);
            putLabel(prefix, isLast, "For " + n->targetName);
            std::string cp = childPrefix(prefix, isLast);
            putLabel(cp, false, "iterable");
            prExpr(n->iterable.get(), childPrefix(cp, false), true);
            putLabel(cp, true, "body");
            prBlock(n->body, childPrefix(cp, true)); break;
        }
        case StmtKind::Def: {
            auto* n = static_cast<const DefStmt*>(s);
            std::string lbl = "Def " + n->name;
            if (!n->typeParams.empty()) {
                lbl += "<";
                for (size_t i = 0; i < n->typeParams.size(); ++i) {
                    if (i) lbl += ", ";
                    lbl += n->typeParams[i];
                }
                lbl += ">";
            }
            lbl += "(";
            for (size_t i = 0; i < n->params.size(); ++i) {
                if (i) lbl += ", ";
                lbl += n->params[i].name;
                if (n->params[i].type && n->params[i].type->kind == ExprKind::NameRef)
                    lbl += ": " + static_cast<const NameRefExpr*>(n->params[i].type.get())->name;
                else if (n->params[i].type) lbl += ": <type>";
            }
            lbl += ")";
            if (n->returnType && n->returnType->kind == ExprKind::NameRef)
                lbl += " -> " + static_cast<const NameRefExpr*>(n->returnType.get())->name;
            else if (n->returnType) lbl += " -> <type>";
            putLabel(prefix, isLast, lbl);
            if (!n->body.stmts.empty())
                prBlock(n->body, childPrefix(prefix, isLast)); break;
        }
        case StmtKind::Return: {
            auto* n = static_cast<const ReturnStmt*>(s);
            putLabel(prefix, isLast, "Return");
            if (n->value) prExpr(n->value.get(), childPrefix(prefix, isLast), true);
            break;
        }
        case StmtKind::Struct: {
            auto* n = static_cast<const StructStmt*>(s);
            putLabel(prefix, isLast, "Struct " + n->name);
            std::string cp = childPrefix(prefix, isLast);
            for (size_t i = 0; i < n->fields.size(); ++i) {
                const auto& f = n->fields[i];
                std::string ft = (f.type && f.type->kind == ExprKind::NameRef)
                    ? static_cast<const NameRefExpr*>(f.type.get())->name : "<type>";
                putLabel(cp, i + 1 == n->fields.size(), f.name + ": " + ft);
            } break;
        }
        case StmtKind::Class: {
            auto* n = static_cast<const ClassStmt*>(s);
            std::string lbl = "Class " + n->name;
            if (!n->typeParams.empty()) {
                lbl += "<";
                for (size_t i = 0; i < n->typeParams.size(); ++i) {
                    if (i) lbl += ", ";
                    lbl += n->typeParams[i];
                }
                lbl += ">";
            }
            if (!n->parentName.empty()) lbl += "(" + n->parentName + ")";
            putLabel(prefix, isLast, lbl);
            std::string cp = childPrefix(prefix, isLast);
            size_t total = n->fields.size() + n->staticFields.size() + n->methods.size();
            size_t idx = 0;
            for (auto& f : n->fields) {
                std::string ft = (f.type && f.type->kind == ExprKind::NameRef)
                    ? static_cast<const NameRefExpr*>(f.type.get())->name : "<type>";
                putLabel(cp, ++idx == total, "field " + f.name + ": " + ft);
            }
            for (auto& sf : n->staticFields) {
                std::string ft = (sf.type && sf.type->kind == ExprKind::NameRef)
                    ? static_cast<const NameRefExpr*>(sf.type.get())->name : "<type>";
                putLabel(cp, ++idx == total, "static " + sf.name + ": " + ft);
            }
            for (auto& m : n->methods)
                putLabel(cp, ++idx == total, "method " + m->name);
            break;
        }
        case StmtKind::Try: {
            auto* n = static_cast<const TryStmt*>(s);
            putLabel(prefix, isLast, "Try");
            std::string cp = childPrefix(prefix, isLast);
            size_t total = 1 + n->handlers.size() + (n->finallyBody ? 1 : 0);
            size_t idx = 0;
            bool bodyLast = (idx + 1 == total); ++idx;
            putLabel(cp, bodyLast, "body");
            prBlock(n->tryBody, childPrefix(cp, bodyLast));
            for (auto& h : n->handlers) {
                bool hl = (idx + 1 == total); ++idx;
                std::string hdr = "except";
                if (h.exceptionType && h.exceptionType->kind == ExprKind::NameRef)
                    hdr += " " + static_cast<const NameRefExpr*>(h.exceptionType.get())->name;
                else if (h.exceptionType) hdr += " <type>";
                if (!h.varName.empty()) hdr += " as " + h.varName;
                putLabel(cp, hl, hdr);
                prBlock(h.body, childPrefix(cp, hl));
            }
            if (n->finallyBody) {
                putLabel(cp, true, "finally");
                prBlock(*n->finallyBody, childPrefix(cp, true));
            }
            break;
        }
        case StmtKind::Raise: {
            auto* n = static_cast<const RaiseStmt*>(s);
            putLabel(prefix, isLast, n->exception ? "Raise" : "Raise (re-raise)");
            if (n->exception)
                prExpr(n->exception.get(), childPrefix(prefix, isLast), true);
            break;
        }
        case StmtKind::Import: {
            auto* n = static_cast<const ImportStmt*>(s);
            std::string lbl = "Import " + n->moduleName;
            if (!n->alias.empty()) lbl += " as " + n->alias;
            putLabel(prefix, isLast, lbl); break;
        }
        case StmtKind::FromImport: {
            auto* n = static_cast<const FromImportStmt*>(s);
            putLabel(prefix, isLast, "FromImport " + n->moduleName);
            std::string cp = childPrefix(prefix, isLast);
            for (size_t i = 0; i < n->items.size(); ++i) {
                bool last = (i + 1 == n->items.size());
                std::string lbl = n->items[i].name;
                if (!n->items[i].alias.empty()) lbl += " as " + n->items[i].alias;
                putLabel(cp, last, lbl);
            }
            break;
        }
        case StmtKind::Const: {
            auto* n = static_cast<const ConstStmt*>(s);
            std::string lbl = "Const " + n->name;
            if (n->type && n->type->kind == ExprKind::NameRef)
                lbl += ": " + static_cast<const NameRefExpr*>(n->type.get())->name;
            putLabel(prefix, isLast, lbl);
            prExpr(n->value.get(), childPrefix(prefix, isLast), true);
            break;
        }
        case StmtKind::Enum: {
            auto* n = static_cast<const EnumStmt*>(s);
            putLabel(prefix, isLast, "Enum " + n->name);
            std::string cp = childPrefix(prefix, isLast);
            for (size_t i = 0; i < n->items.size(); ++i) {
                bool last = (i + 1 == n->items.size());
                putLabel(cp, last, n->items[i].name);
                prExpr(n->items[i].value.get(), childPrefix(cp, last), true);
            }
            break;
        }
        case StmtKind::Yield: {
            auto* n = static_cast<const YieldStmt*>(s);
            putLabel(prefix, isLast, n->value ? "Yield" : "Yield (None)");
            if (n->value)
                prExpr(n->value.get(), childPrefix(prefix, isLast), true);
            break;
        }
        case StmtKind::Pass:     putLabel(prefix, isLast, "Pass"); break;
        case StmtKind::Break:    putLabel(prefix, isLast, "Break"); break;
        case StmtKind::Continue: putLabel(prefix, isLast, "Continue"); break;
        }
    }

    void printProgram(const Block& program) {
        std::printf("Program\n");
        prBlock(program, "");
    }

} // namespace vayu