#include "TypeChecker.hpp"

namespace vayu {

    // ===========================================================================
    // Entry
    // ===========================================================================

    void TypeChecker::check(const Block& program) {
        pushScope();
        installBuiltins();
        installBuiltinExceptions();
        collectSignatures(program);
        for (auto& s : program.stmts) checkStmt(s.get());
        popScope();
    }

    // ===========================================================================
    // Scopes
    // ===========================================================================

    void TypeChecker::pushScope() { scopes_.emplace_back(); }
    void TypeChecker::popScope() { scopes_.pop_back(); }

    void TypeChecker::defineVar(const std::string& n, TypePtr t) {
        scopes_.back().vars[n] = std::move(t);
    }

    TypePtr TypeChecker::lookupUserVar(const std::string& n) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->vars.find(n);
            if (f != it->vars.end()) return f->second;
        }
        return nullptr;
    }

    TypePtr TypeChecker::lookupVar(const std::string& n) {
        if (TypePtr t = lookupUserVar(n)) return t;
        auto bit = builtins_.find(n);
        if (bit != builtins_.end()) return bit->second;
        return nullptr;
    }

    [[noreturn]] void TypeChecker::error(SourceLocation loc, const std::string& msg) {
        throw TypeError(msg, loc);
    }

    // ===========================================================================
    // Builtins
    // ===========================================================================

    void TypeChecker::installBuiltins() {
        auto A = Types::Any();
        auto listAny = Types::List(A);
        auto listStr = Types::List(Types::Str());

        auto B = [&](const char* name, TypePtr t) {
            builtins_[name] = std::move(t);
            };

        // ---- Core ----
        B("print", Types::Function({ A }, Types::None()));
        B("str", Types::Function({ A }, Types::Str()));
        B("int", Types::Function({ A }, Types::Int()));
        B("float", Types::Function({ A }, Types::Float()));
        B("bool", Types::Function({ A }, Types::Bool()));
        B("len", Types::Function({ A }, Types::Int()));
        B("type", Types::Function({ A }, Types::Str()));
        B("abs", Types::Function({ A }, A));
        B("min", Types::Function({ A, A }, A));
        B("max", Types::Function({ A, A }, A));
        B("range", Types::Function({ Types::Int() }, Types::List(Types::Int())));
        B("ord", Types::Function({ Types::Str() }, Types::Int()));
        B("chr", Types::Function({ Types::Int() }, Types::Str()));
        B("list", Types::Function({ A }, listAny));
        // Phase 11.1k1: generator iteration builtin.
        B("next", Types::Function({ A }, A));

        // ---- Higher-order ----
        B("map", Types::Function({ A, listAny }, listAny));
        B("filter", Types::Function({ A, listAny }, listAny));
        B("sorted", Types::Function({ listAny }, listAny));
        B("reduce", Types::Function({ A, listAny }, A));
        B("any", Types::Function({ listAny }, Types::Bool()));
        B("all", Types::Function({ listAny }, Types::Bool()));
        B("sum", Types::Function({ listAny }, A));

        // ---- math module placeholder ----
        B("math", Types::Any());

        // ---- Phase 7A: file I/O ----
        B("read_file", Types::Function({ Types::Str() }, Types::Str()));
        B("write_file", Types::Function({ Types::Str(), Types::Str() }, Types::None()));
        B("file_exists", Types::Function({ Types::Str() }, Types::Bool()));

        // ---- Phase 7A: process + CLI ----
        B("args", Types::Function({}, Types::List(Types::Str())));
        B("run_command", Types::Function({ Types::Str() }, Types::Int()));
        B("exit", Types::Function({ Types::Int() }, Types::None()));
        B("join", Types::Function({ Types::Str(), listStr }, Types::Str()));

        // ---- Phase 7A: input primitives ----
        B("input", Types::Function({}, Types::Str()));
        B("read_line", Types::Function({}, Types::Str()));
        B("read_all", Types::Function({}, Types::Str()));
        B("read_int", Types::Function({}, Types::Int()));
        B("print_raw", Types::Function({ Types::Str() }, Types::None()));

        // ---- Phase 10 builtin modules (name-only registration) ----
        // The compiler's native backend handles these directly.  The type
        // checker just needs to know the names exist and be liberal about
        // their members.
        B("fs", Types::Any());
        B("time", Types::Any());
        B("json", Types::Any());
        B("regex", Types::Any());
        B("thread", Types::Any());
        B("net", Types::Any());
        B("crypto", Types::Any());
        B("random", Types::Any());
        B("os", Types::Any());
    }

    void TypeChecker::installBuiltinExceptions() {
        auto base = Types::Struct("Exception", {});
        base->fields.push_back({ "message", Types::Str() });
        structs_["Exception"] = base;

        auto mkStruct = [&](const std::string& name, TypePtr parent) {
            auto t = Types::Struct(name, {});
            t->parent = std::move(parent);
            structs_[name] = t;
            return t;
            };
        mkStruct("ValueError", base);
        mkStruct("TypeError", base);
        auto rt = mkStruct("RuntimeError", base);
        mkStruct("ZeroDivisionError", rt);
        mkStruct("IndexError", base);
        mkStruct("KeyError", base);
        mkStruct("NameError", base);
        mkStruct("AttributeError", base);
    }

    // ===========================================================================
    // Pass 1 — declarations
    // ===========================================================================

    void TypeChecker::collectSignatures(const Block& program) {
        // ---- structs ----
        for (auto& s : program.stmts) {
            if (s->kind != StmtKind::Struct) continue;
            auto* d = static_cast<const StructStmt*>(s.get());
            if (structs_.count(d->name))
                error(d->loc, "struct '" + d->name + "' already defined");
            auto st = Types::Struct(d->name, {});
            structs_[d->name] = st;
            std::vector<StructFieldInfo> fields;
            for (auto& f : d->fields) {
                for (auto& e : fields) if (e.name == f.name)
                    error(f.loc, "duplicate field '" + f.name +
                        "' in struct '" + d->name + "'");
                fields.push_back({ f.name, resolveTypeExpr(f.type.get()) });
            }
            st->fields = std::move(fields);
        }

        // ---- classes (two passes for inheritance) ----
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& s : program.stmts) {
                if (s->kind != StmtKind::Class) continue;
                auto* d = static_cast<const ClassStmt*>(s.get());

                if (pass == 0) {
                    if (structs_.count(d->name)) continue;
                    auto ct = Types::Struct(d->name, {});
                    structs_[d->name] = ct;
                    continue;
                }
                if (!structs_.count(d->name)) continue;
                auto ct = structs_[d->name];

                if (!d->parentName.empty()) {
                    auto it = structs_.find(d->parentName);
                    if (it == structs_.end())
                        error(d->loc, "unknown parent class '" + d->parentName + "'");
                    if (it->second->kind != TypeKind::Struct)
                        error(d->loc, "'" + d->parentName + "' is not a class");
                    if (it->second->name == d->name)
                        error(d->loc, "class '" + d->name + "' cannot inherit from itself");
                    ct->parent = it->second;
                }

                std::vector<StructFieldInfo> fields;
                for (auto& f : d->fields) {
                    for (auto& e : fields) if (e.name == f.name)
                        error(f.loc, "duplicate field '" + f.name +
                            "' in class '" + d->name + "'");
                    if (ct->findField(f.name))
                        error(f.loc, "field '" + f.name + "' re-declared in subclass");
                    StructFieldInfo fi;
                    fi.name = f.name;
                    fi.type = resolveTypeExpr(f.type.get());
                    fi.vis = (int8_t)visCode(f.vis);
                    fields.push_back(std::move(fi));
                }
                ct->fields = std::move(fields);

                                // Phase 11.1c: statics.
                std::unordered_map<std::string, TypePtr> statics;
                for (auto& sf : d->staticFields) {
                    if (statics.count(sf.name))
                        error(sf.loc, "duplicate static member '" + sf.name +
                            "' in class '" + d->name + "'");
                    if (ct->findField(sf.name))
                        error(sf.loc, "static member '" + sf.name +
                            "' clashes with an instance field");
                    TypePtr st = sf.type ? resolveTypeExpr(sf.type.get()) : Types::Any();
                    statics[sf.name] = st;
                }
                statics_[d->name] = std::move(statics);

                for (auto& m : d->methods) {
                    if (ct->methods.count(m->name))
                        error(m->loc, "method '" + m->name + "' declared twice");
                    if (ct->findField(m->name))
                        error(m->loc, "field '" + m->name +
                            "' already exists; cannot also be a method");
                    std::vector<TypePtr> params;
                    for (size_t i = 0; i < m->params.size(); ++i) {
                        if (i == 0) params.push_back(ct);
                        else params.push_back(m->params[i].type
                            ? resolveTypeExpr(m->params[i].type.get())
                            : Types::Any());
                    }
                    TypePtr ret = m->returnType
                        ? resolveTypeExpr(m->returnType.get())
                        : Types::None();
                    ct->methods[m->name] = Types::Function(std::move(params), ret);
                    ct->methodVis[m->name] = (int8_t)visCode(m->vis);
                }
            }
        }
        // ---- enums (Phase 11.1d) ----
        for (auto& s : program.stmts) {
            if (s->kind != StmtKind::Enum) continue;
            auto* d = static_cast<const EnumStmt*>(s.get());
            if (enums_.count(d->name) || structs_.count(d->name))
                error(d->loc, "'" + d->name + "' already defined");
            std::unordered_map<std::string, long long> items;
            for (auto& it : d->items) {
                // Values were filled in by the parser.  Require IntLit.
                if (!it.value || it.value->kind != ExprKind::IntLit)
                    error(d->loc, "internal: enum item '" + it.name +
                        "' missing int value (parser bug)");
                long long v = static_cast<const IntLitExpr*>(it.value.get())->value;
                items[it.name] = v;
            }
            enums_[d->name] = std::move(items);
        }

        // ---- functions ----
        collectDefs(program, /*isTopLevel=*/true);
    }

    void TypeChecker::collectDefs(const Block& block, bool isTopLevel) {
        for (auto& s : block.stmts) {
            switch (s->kind) {
            case StmtKind::Def: {
                auto* d = static_cast<const DefStmt*>(s.get());

                // Phase 11.2: fresh type-param scope for this signature.
                typeParamScopes_.emplace_back();
                for (auto& tp : d->typeParams) {
                    auto t = std::make_shared<Type>(TypeKind::TypeParam);
                    t->name = tp + "#" + std::to_string(nextTypeParamId_++);
                    typeParamScopes_.back()[tp] = t;
                }

                std::vector<TypePtr> params;
                for (auto& p : d->params)
                    params.push_back(p.type ? resolveTypeExpr(p.type.get())
                        : Types::Any());
                TypePtr ret = d->returnType
                    ? resolveTypeExpr(d->returnType.get())
                    : Types::Any();

                auto sig = Types::Function(std::move(params), std::move(ret));
                sig->typeParams = typeParamScopes_.back();
                typeParamScopes_.pop_back();

                if (isTopLevel && functions_.count(d->name))
                    error(d->loc, "function '" + d->name + "' already defined");

                functions_[d->name] = sig;
                if (isTopLevel) defineVar(d->name, sig);

                collectDefs(d->body, /*isTopLevel=*/false);
                break;
            }
            case StmtKind::If: {
                auto* n = static_cast<const IfStmt*>(s.get());
                collectDefs(n->thenBody, false);
                for (auto& ec : n->elifs) collectDefs(ec.body, false);
                if (n->elseBody) collectDefs(*n->elseBody, false);
                break;
            }
            case StmtKind::While:
                collectDefs(static_cast<const WhileStmt*>(s.get())->body, false);
                break;
            case StmtKind::For:
                collectDefs(static_cast<const ForStmt*>(s.get())->body, false);
                break;
            case StmtKind::Try: {
                auto* n = static_cast<const TryStmt*>(s.get());
                collectDefs(n->tryBody, false);
                for (auto& h : n->handlers) collectDefs(h.body, false);
                if (n->finallyBody) collectDefs(*n->finallyBody, false);
                break;
            }
            case StmtKind::Class: {
                auto* n = static_cast<const ClassStmt*>(s.get());
                for (auto& m : n->methods) collectDefs(m->body, false);
                break;
            }
            default: break;
            }
        }
    }

    // ===========================================================================
    // Type resolution
    // ===========================================================================

    TypePtr TypeChecker::resolveTypeExpr(const Expr* e) {
        if (!e) return Types::Any();

        if (e->kind == ExprKind::GenericType) {
            auto* g = static_cast<const GenericTypeExpr*>(e);
            if (g->name == "list") {
                if (g->typeArgs.size() != 1)
                    error(e->loc, "list<> takes exactly one type argument");
                return Types::List(resolveTypeExpr(g->typeArgs[0].get()));
            }
            if (g->name == "map") {
                if (g->typeArgs.size() != 2)
                    error(e->loc, "map<> takes exactly two type arguments");
                return Types::Map(resolveTypeExpr(g->typeArgs[0].get()),
                    resolveTypeExpr(g->typeArgs[1].get()));
            }
            error(e->loc, "unknown generic type '" + g->name + "'");
        }

        if (e->kind != ExprKind::NameRef) error(e->loc, "expected a type name");

        const std::string& n = static_cast<const NameRefExpr*>(e)->name;

        // Phase 11.2: type params in the current generic signature.
        for (auto it = typeParamScopes_.rbegin();
            it != typeParamScopes_.rend(); ++it) {
            auto found = it->find(n);
            if (found != it->end()) return found->second;
        }

        if (n == "int")   return Types::Int();
        if (n == "int")   return Types::Int();
        if (n == "float") return Types::Float();
        if (n == "bool")  return Types::Bool();
        if (n == "str")   return Types::Str();
        if (n == "char")  return Types::Char();
        if (n == "bytes") return Types::Bytes();
        if (n == "None")  return Types::None();
        if (n == "any")   return Types::Any();
        if (n == "list")  return Types::List(Types::Any());
        if (n == "map")   return Types::Map(Types::Str(), Types::Any());

        auto it = structs_.find(n);
        if (it != structs_.end()) return it->second;

        auto t = std::make_shared<Type>(TypeKind::Named);
        t->name = n;
        return t;
    }

    // ===========================================================================
    // Helpers
    // ===========================================================================

    TypePtr TypeChecker::commonElementType(const TypePtr& a, const TypePtr& b,
        SourceLocation loc) {
        if (a->kind == TypeKind::Error || b->kind == TypeKind::Error)
            return Types::Error();
        if (a->kind == TypeKind::Any) return b;
        if (b->kind == TypeKind::Any) return a;
        if (a->equals(b)) return a;
        bool aN = a->kind == TypeKind::Int || a->kind == TypeKind::Float;
        bool bN = b->kind == TypeKind::Int || b->kind == TypeKind::Float;
        if (aN && bN) {
            if (a->kind == TypeKind::Float || b->kind == TypeKind::Float)
                return Types::Float();
            return Types::Int();
        }
        error(loc, "incompatible element types: " + a->toString() +
            " and " + b->toString());
    }

    TypePtr TypeChecker::lookupCollectionMethod(const TypePtr& target,
        const std::string& name,
        SourceLocation loc) {
        // ---- str ----
        if (target->kind == TypeKind::Str) {
            if (name == "upper" || name == "lower" || name == "strip" ||
                name == "lstrip" || name == "rstrip")
                return Types::Function({}, Types::Str());
            if (name == "split")
                return Types::Function({ Types::Str() }, Types::List(Types::Str()));
            if (name == "join")
                return Types::Function({ Types::List(Types::Str()) }, Types::Str());
            if (name == "replace")
                return Types::Function({ Types::Str(), Types::Str() }, Types::Str());
            if (name == "substr")
                return Types::Function({ Types::Int(), Types::Int() }, Types::Str());
            if (name == "find" || name == "index")
                return Types::Function({ Types::Str() }, Types::Int());
            if (name == "contains" || name == "starts_with" || name == "ends_with")
                return Types::Function({ Types::Str() }, Types::Bool());
            if (name == "is_digit" || name == "is_alpha" || name == "is_space")
                return Types::Function({}, Types::Bool());
            if (name == "char_at")
                return Types::Function({ Types::Int() }, Types::Str());
            if (name == "to_int")
                return Types::Function({}, Types::Int());
            error(loc, "str has no method '" + name + "'");
        }

        // ---- list ----
        if (target->kind == TypeKind::List) {
            TypePtr E = target->params.empty() ? Types::Any() : target->params[0];
            if (name == "append")   return Types::Function({ E }, Types::None());
            if (name == "pop")      return Types::Function({}, E);
            if (name == "clear")    return Types::Function({}, Types::None());
            if (name == "insert")   return Types::Function({ Types::Int(), E }, Types::None());
            if (name == "remove")   return Types::Function({ E }, Types::None());
            if (name == "contains") return Types::Function({ E }, Types::Bool());
            if (name == "index")    return Types::Function({ E }, Types::Int());
            error(loc, "list has no method '" + name + "'");
        }

        // ---- map ----
        if (target->kind == TypeKind::Map) {
            TypePtr K = target->params.size() > 0 ? target->params[0] : Types::Str();
            TypePtr V = target->params.size() > 1 ? target->params[1] : Types::Any();
            if (name == "put")      return Types::Function({ K, V }, Types::None());
            if (name == "get")      return Types::Function({ K }, V);
            if (name == "remove")   return Types::Function({ K }, Types::None());
            if (name == "contains") return Types::Function({ K }, Types::Bool());
            if (name == "keys")     return Types::Function({}, Types::List(K));
            if (name == "values")   return Types::Function({}, Types::List(V));
            if (name == "clear")    return Types::Function({}, Types::None());
            error(loc, "map has no method '" + name + "'");
        }

        return nullptr;
    }

    // ===========================================================================
    // Struct construction
    // ===========================================================================

    TypePtr TypeChecker::checkStructConstruction(const StructStmt* decl,
        const CallExpr* call,
        const std::string& name) {
        TypePtr st = structs_.at(name);
        std::vector<bool> seen(decl->fields.size(), false);
        size_t positional = 0;

        for (const auto& arg : call->args) {
            TypePtr at = checkExpr(arg.value.get());
            if (arg.name.empty()) {
                if (positional >= decl->fields.size())
                    error(arg.loc, "too many positional arguments for struct '" +
                        name + "'");
                TypePtr ft = st->fields[positional].type;
                if (!isAssignable(ft, at))
                    error(arg.loc, "field '" + decl->fields[positional].name +
                        "' expects " + ft->toString() +
                        ", got " + at->toString());
                seen[positional] = true;
                ++positional;
            }
            else {
                int idx = -1;
                for (size_t i = 0; i < decl->fields.size(); ++i)
                    if (decl->fields[i].name == arg.name) { idx = (int)i; break; }
                if (idx < 0)
                    error(arg.loc, "struct '" + name + "' has no field '" +
                        arg.name + "'");
                if (seen[idx])
                    error(arg.loc, "field '" + arg.name + "' given more than once");
                TypePtr ft = st->fields[idx].type;
                if (!isAssignable(ft, at))
                    error(arg.loc, "field '" + arg.name + "' expects " +
                        ft->toString() + ", got " + at->toString());
                seen[idx] = true;
            }
        }
        for (size_t i = 0; i < decl->fields.size(); ++i)
            if (!seen[i])
                error(call->loc, "struct '" + name +
                    "' is missing value for field '" +
                    decl->fields[i].name + "'");
        return st;
    }

    // ===========================================================================
    // Method bodies
    // ===========================================================================
    bool TypeChecker::isSubclassOf(TypePtr sub, TypePtr base) const {
        for (auto c = sub; c; c = c->parent)
            if (c->name == base->name) return true;
        return false;
    }

    void TypeChecker::checkMethodBody(const DefStmt* m, TypePtr cls) {
        auto sig = cls->methods.at(m->name);
        pushScope();
        for (size_t i = 0; i < m->params.size(); ++i)
            defineVar(m->params[i].name, i == 0 ? cls : sig->params[i]);

        TypePtr savedRet = currentReturnType_;
        TypePtr savedClass = currentClass_;
        int     savedLoop = loopDepth_;
        currentReturnType_ = sig->returnType;
        currentClass_ = cls;
        loopDepth_ = 0;

        for (auto& st : m->body.stmts) checkStmt(st.get());

        currentReturnType_ = savedRet;
        currentClass_ = savedClass;
        loopDepth_ = savedLoop;
        popScope();
    }

    // ===========================================================================
    // Statements
    // ===========================================================================

    void TypeChecker::checkBlock(const Block& b) {
        pushScope();
        for (auto& s : b.stmts) checkStmt(s.get());
        popScope();
    }

    void TypeChecker::checkStmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {

        case StmtKind::Expr: {
            auto* n = static_cast<const ExprStmt*>(s);
            checkExpr(n->expr.get());
            return;
        }

        case StmtKind::Assign: {
            auto* n = static_cast<const AssignStmt*>(s);

            if (n->target->kind == ExprKind::NameRef) {
                TypePtr v = checkExpr(n->value.get());
                const auto* nm =
                    static_cast<const NameRefExpr*>(n->target.get());
                TypePtr ex = lookupUserVar(nm->name);
                if (consts_.count(nm->name))
                    error(n->loc, "cannot assign to const '" + nm->name + "'");
                if (ex) {
                    if (!isAssignable(ex, v))
                        error(n->loc, "cannot assign " + v->toString() +
                            " to '" + nm->name + "' of type " +
                            ex->toString());
                }
                else {
                    defineVar(nm->name, v);
                }
                return;
            }

            if (n->target->kind == ExprKind::Attr) {
                auto* a = static_cast<const AttrExpr*>(n->target.get());
                if (a->target->kind == ExprKind::NameRef) {
                    const auto* tn = static_cast<const NameRefExpr*>(a->target.get());
                    auto sit = statics_.find(tn->name);
                    if (sit != statics_.end()) {
                        auto fit = sit->second.find(a->name);
                        if (fit == sit->second.end())
                            error(a->loc, "class '" + tn->name +
                                "' has no static member '" + a->name + "'");
                        TypePtr v = checkExpr(n->value.get());
                        if (!isAssignable(fit->second, v))
                            error(n->loc, "static member '" + a->name +
                                "' expects " + fit->second->toString() +
                                ", got " + v->toString());
                        return;
                    }
                }
                TypePtr t = checkExpr(a->target.get());
                if (t->kind != TypeKind::Struct)
                    error(a->loc, "cannot set field '" + a->name +
                        "' on value of type " + t->toString());
                const StructFieldInfo* f = t->findField(a->name);
                if (!f)
                    error(a->loc, "type '" + t->name + "' has no field '" +
                        a->name + "'");
                TypePtr v = checkExpr(n->value.get());
                if (!isAssignable(f->type, v))
                    error(n->loc, "field '" + a->name + "' expects " +
                        f->type->toString() + ", got " +
                        v->toString());
                return;
            }

            if (n->target->kind == ExprKind::Index) {
                auto* ix = static_cast<const IndexExpr*>(n->target.get());
                TypePtr tgt = checkExpr(ix->target.get());
                TypePtr idx = checkExpr(ix->index.get());
                TypePtr val = checkExpr(n->value.get());

                if (tgt->kind == TypeKind::List) {
                    if (idx->kind != TypeKind::Int && idx->kind != TypeKind::Any)
                        error(ix->loc, "list index must be int, got " +
                            idx->toString());
                    if (!isAssignable(tgt->params[0], val))
                        error(n->loc, "cannot assign " + val->toString() +
                            " into " + tgt->toString());
                    return;
                }
                if (tgt->kind == TypeKind::Map) {
                    if (!isAssignable(tgt->params[0], idx))
                        error(ix->loc, "map key must be " +
                            tgt->params[0]->toString() +
                            ", got " + idx->toString());
                    if (!isAssignable(tgt->params[1], val))
                        error(n->loc, "cannot assign " + val->toString() +
                            " into " + tgt->toString());
                    return;
                }
                if (tgt->kind == TypeKind::Str)
                    error(n->loc, "strings are immutable");
                error(ix->loc, "cannot index-assign to value of type " +
                    tgt->toString());
            }

            error(n->target->loc, "invalid assignment target");
        }

        case StmtKind::AnnotAssign: {
            auto* n = static_cast<const AnnotAssignStmt*>(s);
            TypePtr declared = resolveTypeExpr(n->type.get());
            if (n->value) {
                TypePtr v = checkExpr(n->value.get());
                if (!isAssignable(declared, v))
                    error(n->loc, "cannot initialize '" + n->name + "' (" +
                        declared->toString() +
                        ") with value of type " + v->toString());
            }
            defineVar(n->name, declared);
            return;
        }

        case StmtKind::If: {
            auto* n = static_cast<const IfStmt*>(s);
            checkExpr(n->cond.get());
            checkBlock(n->thenBody);
            for (auto& ec : n->elifs) {
                checkExpr(ec.cond.get());
                checkBlock(ec.body);
            }
            if (n->elseBody) checkBlock(*n->elseBody);
            return;
        }

        case StmtKind::While: {
            auto* n = static_cast<const WhileStmt*>(s);
            checkExpr(n->cond.get());
            ++loopDepth_;
            checkBlock(n->body);
            --loopDepth_;
            return;
        }

        case StmtKind::For: {
            auto* n = static_cast<const ForStmt*>(s);
            TypePtr it = checkExpr(n->iterable.get());

            TypePtr elem;
            if (it->kind == TypeKind::List) {
                elem = it->params.empty() ? Types::Any() : it->params[0];
            }
            else if (it->kind == TypeKind::Map) {
                elem = it->params.empty() ? Types::Any() : it->params[0];
            }
            else if (it->kind == TypeKind::Str) {
                elem = Types::Str();
            }
            else if (it->kind == TypeKind::Any || it->kind == TypeKind::Error) {
                elem = it;
            }
            else {
                error(n->loc, "cannot iterate over value of type " +
                    it->toString());
            }

            defineVar(n->targetName, elem);
            ++loopDepth_;
            for (auto& st : n->body.stmts) checkStmt(st.get());
            --loopDepth_;
            return;
        }

        case StmtKind::Def: {
            auto* n = static_cast<const DefStmt*>(s);
            auto it = functions_.find(n->name);
            if (it == functions_.end())
                error(n->loc, "internal: missing signature for '" +
                    n->name + "'");
            TypePtr sig = it->second;

            defineVar(n->name, sig);

            // Phase 11.2: reuse the signature's type-param scope so `T`
            // resolves to the same TypeParam inside the body.
            typeParamScopes_.push_back(sig->typeParams);

            pushScope();
            for (size_t i = 0; i < n->params.size(); ++i)
                defineVar(n->params[i].name, sig->params[i]);

            TypePtr savedRet = currentReturnType_;
            int     savedLoop = loopDepth_;
            currentReturnType_ = sig->returnType;
            loopDepth_ = 0;

            for (auto& st : n->body.stmts) checkStmt(st.get());

            currentReturnType_ = savedRet;
            loopDepth_ = savedLoop;
            popScope();
            typeParamScopes_.pop_back();
            return;
        }

        case StmtKind::Return: {
            auto* n = static_cast<const ReturnStmt*>(s);
            if (!currentReturnType_)
                error(n->loc, "'return' outside function");
            TypePtr v = n->value ? checkExpr(n->value.get()) : Types::None();
            if (!isAssignable(currentReturnType_, v))
                error(n->loc, "returning " + v->toString() +
                    " from function declared to return " +
                    currentReturnType_->toString());
            return;
        }

        case StmtKind::Struct:
            return;

        case StmtKind::Class: {
            auto* n = static_cast<const ClassStmt*>(s);
            TypePtr ct = structs_.at(n->name);
            for (auto& m : n->methods) checkMethodBody(m.get(), ct);
            return;
        }

        case StmtKind::Try: {
            auto* n = static_cast<const TryStmt*>(s);
            checkBlock(n->tryBody);

            for (auto& h : n->handlers) {
                TypePtr excType = Types::Any();
                if (h.exceptionType) {
                    excType = checkExpr(h.exceptionType.get());
                    if (excType->kind != TypeKind::Struct)
                        error(h.exceptionType->loc,
                            "except type must be an exception class");
                    bool isExc = false;
                    for (TypePtr c = excType; c; c = c->parent)
                        if (c->name == "Exception") { isExc = true; break; }
                    if (!isExc)
                        error(h.exceptionType->loc,
                            "'" + excType->name + "' is not an exception class");
                }
                pushScope();
                if (!h.varName.empty()) defineVar(h.varName, excType);
                for (auto& st : h.body.stmts) checkStmt(st.get());
                popScope();
            }

            if (n->finallyBody) checkBlock(*n->finallyBody);
            return;
        }

        case StmtKind::Raise: {
            auto* n = static_cast<const RaiseStmt*>(s);
            if (n->exception) checkExpr(n->exception.get());
            return;
        }

        case StmtKind::Import: {
            auto* n = static_cast<const ImportStmt*>(s);
            const std::string& bind =
                n->alias.empty() ? n->moduleName : n->alias;
            defineVar(bind, Types::Any());
            return;
        }

        case StmtKind::FromImport: {
            auto* n = static_cast<const FromImportStmt*>(s);
            for (auto& item : n->items) {
                const std::string& bind =
                    item.alias.empty() ? item.name : item.alias;
                defineVar(bind, Types::Any());
            }
            return;
        }
        case StmtKind::Const: {
            auto* n = static_cast<const ConstStmt*>(s);
            if (consts_.count(n->name) || lookupUserVar(n->name) ||
                structs_.count(n->name) || enums_.count(n->name))
                error(n->loc, "'" + n->name + "' already defined");
            TypePtr declared = n->type ? resolveTypeExpr(n->type.get()) : nullptr;
            TypePtr v = checkExpr(n->value.get());
            if (declared && !isAssignable(declared, v))
                error(n->loc, "cannot initialize const '" + n->name + "' (" +
                    declared->toString() + ") with value of type " + v->toString());
            defineVar(n->name, declared ? declared : v);
            consts_.insert(n->name);
            return;
        }

        case StmtKind::Enum:
            // Enum items are type-level; no runtime work here.
            return;

        case StmtKind::Yield: {
            auto* n = static_cast<const YieldStmt*>(s);
            if (n->value) checkExpr(n->value.get());
            return;
        }

        case StmtKind::Pass:
            return;

        case StmtKind::Break:
        case StmtKind::Continue:
            if (loopDepth_ == 0)
                error(s->loc, s->kind == StmtKind::Break
                    ? "'break' outside loop"
                    : "'continue' outside loop");
            return;
        }
    }

    // ===========================================================================
    // Expressions
    // ===========================================================================

    TypePtr TypeChecker::checkExpr(const Expr* e) {
        if (!e) return Types::None();
        switch (e->kind) {
        case ExprKind::IntLit:    return Types::Int();
        case ExprKind::FloatLit:  return Types::Float();
        case ExprKind::StringLit: return Types::Str();
        case ExprKind::CharLit:   return Types::Char();
        case ExprKind::BoolLit:   return Types::Bool();
        case ExprKind::NoneLit:   return Types::None();

        case ExprKind::NameRef: {
            auto* n = static_cast<const NameRefExpr*>(e);
            if (TypePtr t = lookupVar(n->name)) return t;
            auto it = structs_.find(n->name);
            if (it != structs_.end()) return it->second;
            if (enums_.count(n->name))
                error(n->loc, "enum '" + n->name +
                    "' cannot be used as a value; write '" + n->name +
                    ".<item>' instead");
            error(n->loc, "name '" + n->name + "' is not defined");
        }

        case ExprKind::Grouping:
            return checkExpr(static_cast<const GroupingExpr*>(e)->inner.get());

        case ExprKind::ListLit: {
            auto* n = static_cast<const ListLitExpr*>(e);
            if (n->elements.empty()) return Types::List(Types::Any());
            TypePtr elem = checkExpr(n->elements[0].get());
            for (size_t i = 1; i < n->elements.size(); ++i) {
                TypePtr t = checkExpr(n->elements[i].get());
                elem = commonElementType(elem, t, n->elements[i]->loc);
            }
            return Types::List(elem);
        }

        case ExprKind::MapLit: {
            auto* n = static_cast<const MapLitExpr*>(e);
            if (n->entries.empty()) return Types::Map(Types::Str(), Types::Any());
            TypePtr valType = nullptr;
            for (auto& entry : n->entries) {
                TypePtr k = checkExpr(entry.key.get());
                if (k->kind != TypeKind::Str && k->kind != TypeKind::Any)
                    error(entry.key->loc, "map keys must be str, got " +
                        k->toString());
                TypePtr v = checkExpr(entry.value.get());
                if (!valType) valType = v;
                else valType = commonElementType(valType, v, entry.value->loc);
            }
            return Types::Map(Types::Str(), valType);
        }

        case ExprKind::Lambda: {
            auto* n = static_cast<const LambdaExpr*>(e);
            pushScope();
            for (auto& p : n->params) defineVar(p, Types::Any());
            checkExpr(n->body.get());
            popScope();
            return Types::Any();
        }

        case ExprKind::Unary: {
            auto* n = static_cast<const UnaryExpr*>(e);
            TypePtr t = checkExpr(n->operand.get());
            switch (n->op) {
            case UnOp::Not: return Types::Bool();
            case UnOp::Neg: case UnOp::Pos:
                if (t->kind == TypeKind::Int)   return Types::Int();
                if (t->kind == TypeKind::Float) return Types::Float();
                if (t->kind == TypeKind::Any)   return Types::Any();
                if (t->kind == TypeKind::Error) return t;
                error(n->loc, "cannot apply unary operator to " +
                    t->toString());
            }
            return Types::Error();
        }

        case ExprKind::Binary: {
            auto* n = static_cast<const BinaryExpr*>(e);

            if (n->op == BinOp::And || n->op == BinOp::Or) {
                checkExpr(n->lhs.get());
                checkExpr(n->rhs.get());
                return Types::Bool();
            }

            TypePtr lt = checkExpr(n->lhs.get());
            TypePtr rt = checkExpr(n->rhs.get());

            if (lt->kind == TypeKind::Error || rt->kind == TypeKind::Error)
                return Types::Error();

            if (n->op == BinOp::In) {
                if (rt->kind == TypeKind::List || rt->kind == TypeKind::Map) {
                    if (lt->kind != TypeKind::Any && rt->params.size() > 0 &&
                        !isAssignable(rt->params[0], lt))
                        error(n->loc, "cannot check if " + lt->toString() +
                            " is in " + rt->toString());
                    return Types::Bool();
                }
                if (rt->kind == TypeKind::Str && lt->kind == TypeKind::Str)
                    return Types::Bool();
                if (rt->kind == TypeKind::Any) return Types::Bool();
                error(n->loc, "'in' requires a list, map, or str on the right");
            }

            if (lt->kind == TypeKind::Any || rt->kind == TypeKind::Any) {
                switch (n->op) {
                case BinOp::Eq: case BinOp::NotEq:
                case BinOp::Lt: case BinOp::Gt:
                case BinOp::LtEq: case BinOp::GtEq:
                case BinOp::Is: return Types::Bool();
                default: return Types::Any();
                }
            }

            switch (n->op) {
            case BinOp::Add:
                if (lt->kind == TypeKind::Int && rt->kind == TypeKind::Int)
                    return Types::Int();
                if (lt->kind == TypeKind::Float && rt->kind == TypeKind::Float)
                    return Types::Float();
                if (lt->kind == TypeKind::Int && rt->kind == TypeKind::Float)
                    return Types::Float();
                if (lt->kind == TypeKind::Float && rt->kind == TypeKind::Int)
                    return Types::Float();
                if (lt->kind == TypeKind::Str && rt->kind == TypeKind::Str)
                    return Types::Str();
                if (lt->kind == TypeKind::List && rt->kind == TypeKind::List) {
                    if (!lt->params[0]->equals(rt->params[0]))
                        error(n->loc, "cannot concatenate " +
                            lt->toString() + " and " +
                            rt->toString());
                    return lt;
                }
                error(n->loc, "cannot add " + lt->toString() +
                    " and " + rt->toString());
            case BinOp::Sub: {
                TypePtr c = commonNumeric(lt, rt);
                if (c->kind == TypeKind::Error)
                    error(n->loc, "cannot subtract " + rt->toString() +
                        " from " + lt->toString());
                return c;
            }
            case BinOp::Mul:
                if (lt->kind == TypeKind::Str && rt->kind == TypeKind::Int)
                    return Types::Str();
                if (lt->kind == TypeKind::Int && rt->kind == TypeKind::Str)
                    return Types::Str();
                if (lt->kind == TypeKind::List && rt->kind == TypeKind::Int)
                    return lt;
                if (lt->kind == TypeKind::Int && rt->kind == TypeKind::List)
                    return rt;
                {
                    TypePtr c = commonNumeric(lt, rt);
                    if (c->kind == TypeKind::Error)
                        error(n->loc, "cannot multiply " +
                            lt->toString() + " by " +
                            rt->toString());
                    return c;
                }
            case BinOp::Div: {
                bool ln = lt->kind == TypeKind::Int ||
                    lt->kind == TypeKind::Float;
                bool rn = rt->kind == TypeKind::Int ||
                    rt->kind == TypeKind::Float;
                if (!ln || !rn)
                    error(n->loc, "cannot divide " + lt->toString() +
                        " by " + rt->toString());
                return Types::Float();
            }
            case BinOp::FloorDiv: case BinOp::Mod: case BinOp::Pow: {
                TypePtr c = commonNumeric(lt, rt);
                if (c->kind == TypeKind::Error)
                    error(n->loc, std::string("cannot apply '") +
                        binOpName(n->op) + "' to " +
                        lt->toString() + " and " +
                        rt->toString());
                return c;
            }
            case BinOp::Eq: case BinOp::NotEq:
            case BinOp::Lt: case BinOp::Gt:
            case BinOp::LtEq: case BinOp::GtEq:
            case BinOp::Is: return Types::Bool();
            default: return Types::Error();
            }
        }

        case ExprKind::Index: {
            auto* n = static_cast<const IndexExpr*>(e);
            TypePtr tgt = checkExpr(n->target.get());
            TypePtr idx = checkExpr(n->index.get());
            if (tgt->kind == TypeKind::Error || idx->kind == TypeKind::Error)
                return Types::Error();
            if (tgt->kind == TypeKind::Any) return Types::Any();
            if (tgt->kind == TypeKind::List) {
                if (idx->kind != TypeKind::Int && idx->kind != TypeKind::Any)
                    error(n->loc, "list index must be int, got " +
                        idx->toString());
                return tgt->params.empty() ? Types::Any() : tgt->params[0];
            }
            if (tgt->kind == TypeKind::Map) {
                if (!isAssignable(tgt->params[0], idx))
                    error(n->loc, "map key must be " +
                        tgt->params[0]->toString() +
                        ", got " + idx->toString());
                return tgt->params.size() > 1 ? tgt->params[1] : Types::Any();
            }
            if (tgt->kind == TypeKind::Str) {
                if (idx->kind != TypeKind::Int && idx->kind != TypeKind::Any)
                    error(n->loc, "str index must be int, got " +
                        idx->toString());
                return Types::Str();
            }
            error(n->loc, "cannot index value of type " + tgt->toString());
        }

        case ExprKind::Attr: {
            auto* n = static_cast<const AttrExpr*>(e);
            // Enum item access: `Color.Red` -> int
            if (n->target->kind == ExprKind::NameRef) {
                const auto* tn = static_cast<const NameRefExpr*>(n->target.get());
                auto eit = enums_.find(tn->name);
                if (eit != enums_.end()) {
                    if (!eit->second.count(n->name))
                        error(n->loc, "enum '" + tn->name + "' has no item '" +
                            n->name + "'");
                    return Types::Int();
                }
            }
            // Phase 11.1c: ClassName.staticName
            if (n->target->kind == ExprKind::NameRef) {
                const auto* tn = static_cast<const NameRefExpr*>(n->target.get());
                auto sit = statics_.find(tn->name);
                if (sit != statics_.end()) {
                    auto fit = sit->second.find(n->name);
                    if (fit == sit->second.end())
                        error(n->loc, "class '" + tn->name +
                            "' has no static member '" + n->name + "'");
                    return fit->second;
                }
            }
            TypePtr t = checkExpr(n->target.get());
            if (t->kind == TypeKind::Error) return t;
            if (t->kind == TypeKind::Any)   return Types::Any();

            if (t->kind == TypeKind::List || t->kind == TypeKind::Map ||
                t->kind == TypeKind::Str) {
                TypePtr m = lookupCollectionMethod(t, n->name, n->loc);
                if (m) return m;
            }

            if (t->kind != TypeKind::Struct)
                error(n->loc, "cannot read field or method '" + n->name +
                    "' on value of type " + t->toString());
            if (const auto* f = t->findField(n->name)) {
                // Phase 11.1j: enforce visibility.
                if (f->vis != 0) {
                    std::shared_ptr<Type> declaring;
                    for (auto c = t; c; c = c->parent) {
                        if (c->findField(n->name) == f) { declaring = c; break; }
                    }
                    if (declaring) {
                        if (!currentClass_)
                            error(n->loc, "member '" + n->name +
                                "' is not accessible outside its class");
                        bool ok = false;
                        if (f->vis == 2 /*private*/)
                            ok = (currentClass_->name == declaring->name);
                        else /*protected*/
                            ok = isSubclassOf(currentClass_, declaring);
                        if (!ok)
                            error(n->loc, "member '" + n->name +
                                "' is not accessible from class '" +
                                currentClass_->name + "'");
                    }
                }
                return f->type;
            }
            if (TypePtr m = t->findMethod(n->name)) {
                // Visibility.
                int8_t code = 0;
                std::shared_ptr<Type> declaring;
                for (auto c = t; c; c = c->parent) {
                    auto it = c->methodVis.find(n->name);
                    if (it != c->methodVis.end()) {
                        code = it->second; declaring = c; break;
                    }
                }
                if (code != 0 && declaring) {
                    if (!currentClass_)
                        error(n->loc, "method '" + n->name +
                            "' is not accessible outside its class");
                    bool ok = false;
                    if (code == 2 /*private*/)
                        ok = (currentClass_->name == declaring->name);
                    else
                        ok = isSubclassOf(currentClass_, declaring);
                    if (!ok)
                        error(n->loc, "method '" + n->name +
                            "' is not accessible from class '" +
                            currentClass_->name + "'");
                }
                std::vector<TypePtr> bound(m->params.begin() + 1,
                    m->params.end());
                return Types::Function(std::move(bound), m->returnType);
            }
            error(n->loc, "type '" + t->name + "' has no field or method '" +
                n->name + "'");
        }

        case ExprKind::Call: {
            auto* n = static_cast<const CallExpr*>(e);

            if (n->callee->kind == ExprKind::NameRef) {
                const auto* nm =
                    static_cast<const NameRefExpr*>(n->callee.get());

                if (nm->name == "super") {
                    if (!currentClass_)
                        error(n->loc, "'super()' outside method");
                    if (!currentClass_->parent)
                        error(n->loc, "class '" + currentClass_->name +
                            "' has no parent");
                    return currentClass_->parent;
                }

                auto sit = structs_.find(nm->name);
                if (sit != structs_.end()) {
                    TypePtr ct = sit->second;
                    TypePtr initSig = ct->findMethod("__init__");
                    if (initSig) {
                        for (const auto& a : n->args)
                            if (!a.name.empty())
                                error(a.loc, "class '" + nm->name +
                                    "' __init__ does not accept keyword arguments");
                        if (n->args.size() != initSig->params.size() - 1)
                            error(n->loc, "class '" + nm->name +
                                "' constructor expects " +
                                std::to_string(initSig->params.size() - 1) +
                                " argument(s), got " +
                                std::to_string(n->args.size()));
                        for (size_t i = 0; i < n->args.size(); ++i) {
                            TypePtr at = checkExpr(n->args[i].value.get());
                            if (!isAssignable(initSig->params[i + 1], at))
                                error(n->args[i].loc,
                                    "argument " + std::to_string(i + 1) +
                                    ": expected " +
                                    initSig->params[i + 1]->toString() +
                                    ", got " + at->toString());
                        }
                        return ct;
                    }
                    std::vector<StructFieldInfo> all;
                    for (TypePtr c = ct; c; c = c->parent)
                        for (auto& f : c->fields) all.push_back(f);

                    std::vector<bool> seen(all.size(), false);
                    size_t pos = 0;
                    for (const auto& arg : n->args) {
                        TypePtr at = checkExpr(arg.value.get());
                        if (arg.name.empty()) {
                            if (pos >= all.size())
                                error(arg.loc, "too many positional arguments");
                            if (!isAssignable(all[pos].type, at))
                                error(arg.loc, "field '" + all[pos].name +
                                    "' expects " +
                                    all[pos].type->toString() +
                                    ", got " + at->toString());
                            seen[pos] = true;
                            ++pos;
                        }
                        else {
                            int idx = -1;
                            for (size_t i = 0; i < all.size(); ++i)
                                if (all[i].name == arg.name) {
                                    idx = (int)i; break;
                                }
                            if (idx < 0)
                                error(arg.loc, "no field '" + arg.name + "'");
                            if (seen[idx])
                                error(arg.loc, "field given twice");
                            if (!isAssignable(all[idx].type, at))
                                error(arg.loc, "field '" + arg.name +
                                    "' expects " +
                                    all[idx].type->toString() +
                                    ", got " + at->toString());
                            seen[idx] = true;
                        }
                    }
                    for (size_t i = 0; i < all.size(); ++i)
                        if (!seen[i])
                            error(n->loc, "missing field '" + all[i].name + "'");
                    return ct;
                }
            }

            TypePtr callee = checkExpr(n->callee.get());

            // Methods with optional trailing args: split/strip.
            // But not for builtin modules — regex.split takes 2 args, and
            // the native backend routes .split calls to regex_split, not to
            // the str.split rule.  Skip this special-case when the receiver
            // is a builtin module name.
            if (n->callee->kind == ExprKind::Attr) {
                auto* attr = static_cast<const AttrExpr*>(n->callee.get());
                bool fromBuiltinModule = false;
                if (attr->target->kind == ExprKind::NameRef) {
                    const auto* tgt = static_cast<const NameRefExpr*>(
                        attr->target.get());
                    const std::string& tname = tgt->name;
                    if (tname == "fs" || tname == "time" || tname == "json" ||
                        tname == "regex" || tname == "thread") {
                        fromBuiltinModule = true;
                    }
                }
                if (!fromBuiltinModule &&
                    (attr->name == "split" || attr->name == "strip")) {
                    std::vector<TypePtr> argTypes;
                    for (auto& a : n->args)
                        argTypes.push_back(checkExpr(a.value.get()));
                    if (argTypes.size() > 1)
                        error(n->loc, "method takes 0 or 1 argument(s), got " +
                            std::to_string(argTypes.size()));
                    if (argTypes.size() == 1 &&
                        !isAssignable(Types::Str(), argTypes[0]))
                        error(n->args[0].loc, "argument must be str, got " +
                            argTypes[0]->toString());
                    return callee->returnType ? callee->returnType
                        : Types::None();
                }
            }

            std::vector<TypePtr> argTypes;
            for (auto& a : n->args) argTypes.push_back(checkExpr(a.value.get()));

            if (callee->kind == TypeKind::Error) return Types::Error();
            if (callee->kind == TypeKind::Any)   return Types::Any();
            if (callee->kind != TypeKind::Function)
                error(n->loc, "cannot call value of type " + callee->toString());

            const NameRefExpr* nm =
                (n->callee->kind == ExprKind::NameRef)
                ? static_cast<const NameRefExpr*>(n->callee.get())
                : nullptr;
            if (nm && (nm->name == "print" || nm->name == "min" ||
                nm->name == "max" || nm->name == "range" ||
                nm->name == "list" || nm->name == "sorted" ||
                nm->name == "input"))
                return callee->returnType ? callee->returnType : Types::None();

            if (argTypes.size() != callee->params.size())
                error(n->loc, "function expects " +
                    std::to_string(callee->params.size()) +
                    " argument(s), got " +
                    std::to_string(argTypes.size()));

            // Phase 11.2: infer type-param substitutions from arg types.
            std::unordered_map<std::string, TypePtr> subst;
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (!unify(callee->params[i], argTypes[i], subst))
                    error(n->args[i].loc, "argument " +
                        std::to_string(i + 1) + ": expected " +
                        callee->params[i]->toString() +
                        ", got " + argTypes[i]->toString());
            }
            // Unresolved type params default to Any.
            for (auto& [k, v] : callee->typeParams) {
                if (subst.find(v->name) == subst.end())
                    subst[v->name] = Types::Any();
            }
            TypePtr result = callee->returnType
                ? substitute(callee->returnType, subst)
                : Types::None();
            return result;
        }

        case ExprKind::GenericType:
            error(e->loc, "generic type expression used where a value was expected");
        }
        return Types::Error();
    }

    bool TypeChecker::unify(const TypePtr& pattern, const TypePtr& actual,
        std::unordered_map<std::string, TypePtr>& subst) {
        if (!pattern || !actual) return false;
        if (pattern->kind == TypeKind::Error || actual->kind == TypeKind::Error)
            return true;

        if (pattern->kind == TypeKind::TypeParam) {
            auto it = subst.find(pattern->name);
            if (it == subst.end()) {
                subst[pattern->name] = actual;
                return true;
            }
            return isAssignable(it->second, actual) &&
                isAssignable(actual, it->second);
        }
        if (pattern->kind == TypeKind::Any || actual->kind == TypeKind::Any)
            return true;
        if (pattern->kind == TypeKind::List && actual->kind == TypeKind::List) {
            if (pattern->params.empty() || actual->params.empty()) return true;
            return unify(pattern->params[0], actual->params[0], subst);
        }
        if (pattern->kind == TypeKind::Map && actual->kind == TypeKind::Map) {
            if (pattern->params.size() < 2 || actual->params.size() < 2)
                return true;
            return unify(pattern->params[0], actual->params[0], subst) &&
                unify(pattern->params[1], actual->params[1], subst);
        }
        return isAssignable(pattern, actual);
    }

    TypePtr TypeChecker::substitute(
        const TypePtr& t,
        const std::unordered_map<std::string, TypePtr>& subst) {
        if (!t) return t;
        if (t->kind == TypeKind::TypeParam) {
            auto it = subst.find(t->name);
            return it != subst.end() ? it->second : Types::Any();
        }
        if (t->kind == TypeKind::List) {
            auto r = std::make_shared<Type>(TypeKind::List);
            r->params.push_back(substitute(
                t->params.empty() ? Types::Any() : t->params[0], subst));
            return r;
        }
        if (t->kind == TypeKind::Map) {
            auto r = std::make_shared<Type>(TypeKind::Map);
            r->params.push_back(substitute(
                t->params.size() > 0 ? t->params[0] : Types::Any(), subst));
            r->params.push_back(substitute(
                t->params.size() > 1 ? t->params[1] : Types::Any(), subst));
            return r;
        }
        return t;
    }

} // namespace vayu