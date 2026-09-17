#include "Compiler.hpp"

namespace vayu {

    // ===========================================================================
    // Entry
    // ===========================================================================

    void Compiler::compile(const Block& program, Chunk& out) {
        chunk_ = &out;
        collectDeclarations(program);
        resolveAllFields();
        compileBlock(program);
        chunk_->emitOp(OpCode::NONE, 0);
        chunk_->emitOp(OpCode::RETURN_V, 0);
        chunk_ = nullptr;
    }

    [[noreturn]] void Compiler::error(SourceLocation loc, const std::string& msg) {
        throw CompileError(msg, loc);
    }

    // ===========================================================================
    // Declaration pre-pass
    // ===========================================================================

    void Compiler::collectDeclarations(const Block& program) {
        // ---- structs ----
        for (auto& s : program.stmts) {
            if (s->kind == StmtKind::Struct) {
                auto* d = static_cast<const StructStmt*>(s.get());
                ClassInfo info;
                for (auto& f : d->fields) info.ownFields.push_back(f.name);
                classInfo_[d->name] = std::move(info);
            }
        }

        // ---- classes ----
        for (auto& s : program.stmts) {
            if (s->kind != StmtKind::Class) continue;
            auto* d = static_cast<const ClassStmt*>(s.get());
            ClassInfo info;
            info.parentName = d->parentName;
            for (auto& f : d->fields) info.ownFields.push_back(f.name);
            for (auto& m : d->methods) {
                if (m->name == "__init__") {
                    info.hasInit = true;
                    for (size_t i = 1; i < m->params.size(); ++i)
                        info.initParams.push_back(m->params[i].name);
                    break;
                }
            }
            classInfo_[d->name] = std::move(info);

            // Phase 11.1c: register static member names.
            std::unordered_map<std::string, bool> sm;
            for (auto& sf : d->staticFields) sm[sf.name] = true;
            statics_[d->name] = std::move(sm);
        }

        // ---- propagate inherited __init__ signatures ----
        for (int pass = 0; pass < 8; ++pass) {
            bool changed = false;
            for (auto& [name, info] : classInfo_) {
                if (info.hasInit) continue;
                if (info.parentName.empty()) continue;
                auto pit = classInfo_.find(info.parentName);
                if (pit == classInfo_.end()) continue;
                if (!pit->second.hasInit) continue;
                info.hasInit = true;
                info.initParams = pit->second.initParams;
                changed = true;
            }
            if (!changed) break;
        }

        // ---- Phase 11.1d: enums ----
        for (auto& s : program.stmts) {
            if (s->kind != StmtKind::Enum) continue;
            auto* d = static_cast<const EnumStmt*>(s.get());
            std::unordered_map<std::string, long long> items;
            for (auto& it : d->items) {
                long long v = 0;
                if (it.value && it.value->kind == ExprKind::IntLit)
                    v = static_cast<const IntLitExpr*>(it.value.get())->value;
                items[it.name] = v;
            }
            enums_[d->name] = std::move(items);
        }
    }

    // ===========================================================================
    // Field resolution
    // ===========================================================================

    std::vector<std::string> Compiler::resolveFields(
        const std::string& name, std::unordered_set<std::string>& visiting) {
        auto it = classInfo_.find(name);
        if (it == classInfo_.end()) return {};
        if (visiting.count(name)) return {};
        visiting.insert(name);
        std::vector<std::string> result;
        if (!it->second.parentName.empty())
            result = resolveFields(it->second.parentName, visiting);
        for (auto& f : it->second.ownFields) result.push_back(f);
        visiting.erase(name);
        return result;
    }

    void Compiler::resolveAllFields() {
        for (auto& [name, info] : classInfo_) {
            std::unordered_set<std::string> visiting;
            info.allFields = resolveFields(name, visiting);
        }
    }

    // ===========================================================================
    // Emit helpers
    // ===========================================================================

    size_t Compiler::emitJump(OpCode op, int line) {
        chunk_->emitOp(op, line);
        size_t pos = chunk_->code.size();
        chunk_->emit(0, line);
        chunk_->emit(0, line);
        return pos;
    }

    void Compiler::patchJump(size_t operandPos, size_t target) {
        chunk_->patchJump(operandPos, (int)target, 0);
    }

    void Compiler::emitLoop(size_t loopStart, int line) {
        chunk_->emitOp(OpCode::JUMP, line);
        size_t pos = chunk_->code.size();
        chunk_->emit(0, line);
        chunk_->emit(0, line);
        chunk_->patchJump(pos, (int)loopStart, line);
    }

    void Compiler::emitJumpTo(size_t target, int line) {
        chunk_->emitOp(OpCode::JUMP, line);
        size_t pos = chunk_->code.size();
        chunk_->emit(0, line);
        chunk_->emit(0, line);
        chunk_->patchJump(pos, (int)target, line);
    }

    void Compiler::emitNameU16(OpCode op, const std::string& name, int line) {
        chunk_->emitOp(op, line);
        int idx = chunk_->addName(name);
        chunk_->emit((uint8_t)((idx >> 8) & 0xFF), line);
        chunk_->emit((uint8_t)(idx & 0xFF), line);
    }

    void Compiler::emitNameU16WithCount(OpCode op, const std::string& name,
        uint8_t count, int line) {
        chunk_->emitOp(op, line);
        int idx = chunk_->addName(name);
        chunk_->emit((uint8_t)((idx >> 8) & 0xFF), line);
        chunk_->emit((uint8_t)(idx & 0xFF), line);
        chunk_->emit(count, line);
    }

    // ===========================================================================
    // Statements
    // ===========================================================================

    void Compiler::compileBlock(const Block& b) {
        for (auto& s : b.stmts) compileStmt(s.get());
    }

    void Compiler::compileStmt(const Stmt* s) {
        if (!s) return;
        int line = s->loc.line;

        switch (s->kind) {

        case StmtKind::Expr: {
            auto* n = static_cast<const ExprStmt*>(s);
            compileExpr(n->expr.get());
            chunk_->emitOp(OpCode::POP, line);
            return;
        }

        case StmtKind::Assign: {
            auto* n = static_cast<const AssignStmt*>(s);

            if (n->target->kind == ExprKind::NameRef) {
                const auto* nm = static_cast<const NameRefExpr*>(n->target.get());
                compileExpr(n->value.get());
                emitNameU16(OpCode::DEFINE, nm->name, line);
                return;
            }
            if (n->target->kind == ExprKind::Attr) {
                auto* a = static_cast<const AttrExpr*>(n->target.get());
                // Phase 11.1c: ClassName.staticName = value
                if (a->target->kind == ExprKind::NameRef) {
                    const auto* tn =
                        static_cast<const NameRefExpr*>(a->target.get());
                    auto sit = statics_.find(tn->name);
                    if (sit != statics_.end() && sit->second.count(a->name)) {
                        compileExpr(n->value.get());
                        emitNameU16(OpCode::DEFINE,
                            staticName(tn->name, a->name), line);
                        return;
                    }
                }
                compileAssignAttr(a, n->value.get(), line);
                return;
            }
            if (n->target->kind == ExprKind::Index) {
                auto* ix = static_cast<const IndexExpr*>(n->target.get());
                compileExpr(ix->target.get());
                compileExpr(ix->index.get());
                compileExpr(n->value.get());
                chunk_->emitOp(OpCode::INDEX_SET, line);
                return;
            }
            error(n->target->loc, "VM mode: unsupported assignment target");
        }

        case StmtKind::AnnotAssign: {
            auto* n = static_cast<const AnnotAssignStmt*>(s);
            if (n->value) compileExpr(n->value.get());
            else          chunk_->emitOp(OpCode::NONE, line);
            emitNameU16(OpCode::DEFINE, n->name, line);
            return;
        }

        case StmtKind::Const: {
            auto* n = static_cast<const ConstStmt*>(s);
            compileExpr(n->value.get());
            emitNameU16(OpCode::DEFINE, n->name, line);
            return;
        }

        case StmtKind::Enum:
            return;   // type-level; no bytecode

        case StmtKind::Class: {
            // Phase 11.1c: emit static initializers at class-statement time.
            auto* n = static_cast<const ClassStmt*>(s);
            for (auto& sf : n->staticFields) {
                if (sf.init) compileExpr(sf.init.get());
                else         chunk_->emitOp(OpCode::NONE, line);
                emitNameU16(OpCode::DEFINE, staticName(n->name, sf.name), line);
            }
            return;
        }

        case StmtKind::If:     compileIf(static_cast<const IfStmt*>(s));     return;
        case StmtKind::While:  compileWhile(static_cast<const WhileStmt*>(s));  return;
        case StmtKind::For:    compileFor(static_cast<const ForStmt*>(s));    return;
        case StmtKind::Def:    compileDef(static_cast<const DefStmt*>(s));    return;
        case StmtKind::Return: compileReturn(static_cast<const ReturnStmt*>(s)); return;
        case StmtKind::Try:    compileTry(static_cast<const TryStmt*>(s));    return;
        case StmtKind::Raise:  compileRaise(static_cast<const RaiseStmt*>(s));  return;
        case StmtKind::Yield: {
            auto* n = static_cast<const YieldStmt*>(s);
            if (n->value) compileExpr(n->value.get());
            else          chunk_->emitOp(OpCode::NONE, line);
            chunk_->emitOp(OpCode::YIELD_V, line);
            return;
        }

        case StmtKind::Import:
            compileImport(static_cast<const ImportStmt*>(s));
            return;

        case StmtKind::FromImport:
            compileFrom(static_cast<const FromImportStmt*>(s));
            return;

        case StmtKind::Break:
            if (loopStack_.empty())
                error(s->loc, "'break' outside loop");
            loopStack_.back().breakJumps.push_back(emitJump(OpCode::JUMP, line));
            return;

        case StmtKind::Continue:
            if (loopStack_.empty())
                error(s->loc, "'continue' outside loop");
            emitJumpTo(loopStack_.back().continueTarget, line);
            return;

        case StmtKind::Pass: return;

        case StmtKind::Struct:
            return;   // registered at compile time
        }
    }

    void Compiler::compileImport(const ImportStmt* n) {
        int line = n->loc.line;
        emitNameU16(OpCode::IMPORT, n->moduleName, line);
        const std::string& bind = n->alias.empty() ? n->moduleName : n->alias;
        emitNameU16(OpCode::DEFINE, bind, line);
    }

    void Compiler::compileFrom(const FromImportStmt* n) {
        int line = n->loc.line;
        emitNameU16(OpCode::IMPORT, n->moduleName, line);
        for (auto& item : n->items) {
            emitNameU16(OpCode::IMPORT_MEMBER, item.name, line);
            const std::string& bind = item.alias.empty() ? item.name : item.alias;
            emitNameU16(OpCode::DEFINE, bind, line);
        }
        chunk_->emitOp(OpCode::POP, line);
    }

    void Compiler::compileAssignAttr(const AttrExpr* target, const Expr* value,
        int line) {
        compileExpr(target->target.get());
        compileExpr(value);
        emitNameU16(OpCode::ATTR_SET, target->name, line);
    }

    // ===========================================================================
    // Functions & lambdas
    // ===========================================================================

    void Compiler::compileDef(const DefStmt* n) {
        int line = n->loc.line;
        auto fnChunk = std::make_shared<Chunk>();
        for (auto& p : n->params) fnChunk->paramNames.push_back(p.name);

        Chunk* saved = chunk_;
        chunk_ = fnChunk.get();
        for (auto& st : n->body.stmts) compileStmt(st.get());
        chunk_->emitOp(OpCode::NONE, line);
        chunk_->emitOp(OpCode::RETURN_V, line);
        chunk_ = saved;

        int fnIdx = chunk_->addFunction(fnChunk);
        chunk_->emitOp(n->isGenerator ? OpCode::MAKE_GENERATOR : OpCode::MAKE_FN, line);
        chunk_->emit((uint8_t)((fnIdx >> 8) & 0xFF), line);
        chunk_->emit((uint8_t)(fnIdx & 0xFF), line);
        emitNameU16(OpCode::DEFINE, n->name, line);
    }

    void Compiler::compileLambda(const LambdaExpr* n) {
        int line = n->loc.line;
        auto fnChunk = std::make_shared<Chunk>();
        for (auto& p : n->params) fnChunk->paramNames.push_back(p);

        Chunk* saved = chunk_;
        chunk_ = fnChunk.get();
        compileExpr(n->body.get());
        chunk_->emitOp(OpCode::RETURN_V, line);
        chunk_ = saved;

        int fnIdx = chunk_->addFunction(fnChunk);
        chunk_->emitOp(OpCode::MAKE_FN, line);
        chunk_->emit((uint8_t)((fnIdx >> 8) & 0xFF), line);
        chunk_->emit((uint8_t)(fnIdx & 0xFF), line);
    }

    void Compiler::compileReturn(const ReturnStmt* n) {
        int line = n->loc.line;
        if (n->value) compileExpr(n->value.get());
        else          chunk_->emitOp(OpCode::NONE, line);
        chunk_->emitOp(OpCode::RETURN_V, line);
    }

    // ===========================================================================
    // Control flow
    // ===========================================================================

    void Compiler::compileIf(const IfStmt* n) {
        int line = n->loc.line;
        std::vector<size_t> endJumps;

        compileExpr(n->cond.get());
        size_t skipThen = emitJump(OpCode::JUMP_IF_FALSE, line);
        compileBlock(n->thenBody);
        endJumps.push_back(emitJump(OpCode::JUMP, line));
        patchJump(skipThen, chunk_->here());

        for (auto& ec : n->elifs) {
            compileExpr(ec.cond.get());
            size_t s = emitJump(OpCode::JUMP_IF_FALSE, line);
            compileBlock(ec.body);
            endJumps.push_back(emitJump(OpCode::JUMP, line));
            patchJump(s, chunk_->here());
        }

        if (n->elseBody) compileBlock(*n->elseBody);

        size_t end = chunk_->here();
        for (size_t p : endJumps) patchJump(p, end);
    }

    void Compiler::compileWhile(const WhileStmt* n) {
        int line = n->loc.line;
        size_t loopStart = chunk_->here();

        loopStack_.push_back({});
        loopStack_.back().continueTarget = loopStart;

        compileExpr(n->cond.get());
        size_t exitJump = emitJump(OpCode::JUMP_IF_FALSE, line);
        compileBlock(n->body);
        emitLoop(loopStart, line);

        size_t exit = chunk_->here();
        patchJump(exitJump, exit);
        for (size_t j : loopStack_.back().breakJumps) patchJump(j, exit);
        loopStack_.pop_back();
    }

    void Compiler::compileFor(const ForStmt* n) {
        int line = n->loc.line;
        compileExpr(n->iterable.get());
        chunk_->emitOp(OpCode::ITER_NEW, line);

        size_t loopStart = chunk_->here();

        loopStack_.push_back({});
        loopStack_.back().continueTarget = loopStart;

        chunk_->emitOp(OpCode::ITER_NEXT, line);
        size_t iterOperandPos = chunk_->code.size();
        chunk_->emit(0, line);
        chunk_->emit(0, line);

        emitNameU16(OpCode::DEFINE, n->targetName, line);

        compileBlock(n->body);
        emitLoop(loopStart, line);

        size_t exit = chunk_->here();
        patchJump(iterOperandPos, exit);
        for (size_t j : loopStack_.back().breakJumps) patchJump(j, exit);
        loopStack_.pop_back();

        chunk_->emitOp(OpCode::POP, line);
        chunk_->emitOp(OpCode::POP, line);
    }

    // ===========================================================================
    // try / raise
    // ===========================================================================

    void Compiler::compileTry(const TryStmt* t) {
        int line = t->loc.line;

        chunk_->emitOp(OpCode::TRY_BEGIN, line);
        size_t operandPos = chunk_->code.size();
        chunk_->emit(0, line);
        chunk_->emit(0, line);

        compileBlock(t->tryBody);
        chunk_->emitOp(OpCode::TRY_END, line);
        size_t jumpAfterTry = emitJump(OpCode::JUMP, line);

        size_t catchIp = chunk_->here();
        patchJump(operandPos, catchIp);

        std::vector<size_t> doneJumps;
        for (auto& h : t->handlers) {
            size_t skip = 0;
            if (h.exceptionType) {
                if (h.exceptionType->kind != ExprKind::NameRef)
                    error(h.exceptionType->loc,
                        "try/except: exception type must be a class name");
                const std::string& cn =
                    static_cast<const NameRefExpr*>(h.exceptionType.get())->name;

                chunk_->emitOp(OpCode::EXCEPT_MATCH, line);
                int ni = chunk_->addName(cn);
                chunk_->emit((uint8_t)((ni >> 8) & 0xFF), line);
                chunk_->emit((uint8_t)(ni & 0xFF), line);
                skip = emitJump(OpCode::JUMP_IF_FALSE, line);
            }
            chunk_->emitOp(OpCode::EXCEPT_PUSH, line);
            if (!h.varName.empty()) {
                chunk_->emitOp(OpCode::DUP, line);
                emitNameU16(OpCode::DEFINE, h.varName, line);
            }
            compileBlock(h.body);
            chunk_->emitOp(OpCode::EXCEPT_POP, line);
            chunk_->emitOp(OpCode::POP, line);
            doneJumps.push_back(emitJump(OpCode::JUMP, line));
            if (skip) patchJump(skip, chunk_->here());
        }
        chunk_->emitOp(OpCode::RAISE, line);

        size_t afterCatch = chunk_->here();
        patchJump(jumpAfterTry, afterCatch);
        for (size_t j : doneJumps) patchJump(j, afterCatch);

        if (t->finallyBody) compileBlock(*t->finallyBody);
    }

    void Compiler::compileRaise(const RaiseStmt* n) {
        int line = n->loc.line;
        if (n->exception) {
            compileExpr(n->exception.get());
            chunk_->emitOp(OpCode::RAISE, line);
        }
        else {
            chunk_->emitOp(OpCode::RERAISE, line);
        }
    }

    // ===========================================================================
    // Expressions
    // ===========================================================================

    void Compiler::compileAttrGet(const AttrExpr* a, int line) {
        compileExpr(a->target.get());
        emitNameU16(OpCode::ATTR_GET, a->name, line);
    }

    void Compiler::compileCall(const CallExpr* c, int line) {
        if (c->callee->kind == ExprKind::NameRef) {
            const auto* nm = static_cast<const NameRefExpr*>(c->callee.get());
            if (nm->name == "super") {
                if (!c->args.empty())
                    error(c->loc, "super() takes no arguments");
                chunk_->emitOp(OpCode::SUPER, line);
                return;
            }
        }

        if (c->callee->kind == ExprKind::NameRef) {
            const auto* nm = static_cast<const NameRefExpr*>(c->callee.get());
            auto ci = classInfo_.find(nm->name);
            if (ci != classInfo_.end()) {
                const ClassInfo& info = ci->second;

                if (info.hasInit) {
                    for (auto& a : c->args)
                        if (!a.name.empty())
                            error(a.loc,
                                "VM mode: keyword args for class with __init__ unsupported");
                    if (c->args.size() != info.initParams.size())
                        error(c->loc, "class '" + nm->name + "' constructor expects " +
                            std::to_string(info.initParams.size()) +
                            " argument(s), got " +
                            std::to_string(c->args.size()));
                    for (auto& a : c->args) compileExpr(a.value.get());
                    emitNameU16WithCount(OpCode::NEW_INSTANCE, nm->name,
                        (uint8_t)c->args.size(), line);
                    return;
                }

                std::vector<int> argForField(info.allFields.size(), -1);
                size_t positional = 0;
                for (size_t i = 0; i < c->args.size(); ++i) {
                    const auto& a = c->args[i];
                    if (a.name.empty()) {
                        if (positional >= info.allFields.size())
                            error(a.loc, "too many positional arguments for '" +
                                nm->name + "'");
                        argForField[positional] = (int)i;
                        ++positional;
                    }
                    else {
                        int fi = -1;
                        for (size_t j = 0; j < info.allFields.size(); ++j)
                            if (info.allFields[j] == a.name) { fi = (int)j; break; }
                        if (fi < 0)
                            error(a.loc, "'" + nm->name + "' has no field '" +
                                a.name + "'");
                        if (argForField[fi] != -1)
                            error(a.loc, "field '" + a.name + "' given twice");
                        argForField[fi] = (int)i;
                    }
                }
                for (size_t j = 0; j < info.allFields.size(); ++j) {
                    if (argForField[j] == -1)
                        error(c->loc, "'" + nm->name +
                            "' is missing value for field '" +
                            info.allFields[j] + "'");
                    compileExpr(c->args[argForField[j]].value.get());
                }
                emitNameU16WithCount(OpCode::NEW_INSTANCE, nm->name,
                    (uint8_t)info.allFields.size(), line);
                return;
            }
        }

        if (c->callee->kind == ExprKind::Attr) {
            auto* attr = static_cast<const AttrExpr*>(c->callee.get());
            compileExpr(attr->target.get());
            emitNameU16(OpCode::ATTR_GET, attr->name, line);
            for (auto& a : c->args) {
                if (!a.name.empty())
                    error(a.loc, "VM mode: keyword args for method calls unsupported");
                compileExpr(a.value.get());
            }
            chunk_->emitOp(OpCode::CALL, line);
            chunk_->emit((uint8_t)c->args.size(), line);
            return;
        }

        if (c->callee->kind == ExprKind::NameRef) {
            compileExpr(c->callee.get());
            for (auto& a : c->args) {
                if (!a.name.empty())
                    error(a.loc, "VM mode: keyword arguments not yet supported");
                compileExpr(a.value.get());
            }
            chunk_->emitOp(OpCode::CALL, line);
            chunk_->emit((uint8_t)c->args.size(), line);
            return;
        }

        error(c->loc, "VM mode: unsupported call target");
    }

    void Compiler::compileExpr(const Expr* e) {
        if (!e) return;
        int line = e->loc.line;

        switch (e->kind) {
        case ExprKind::IntLit: {
            auto* n = static_cast<const IntLitExpr*>(e);
            int idx = chunk_->addConstant(Value((long long)n->value));
            chunk_->emitOp(OpCode::CONST, line);
            chunk_->emit((uint8_t)((idx >> 8) & 0xFF), line);
            chunk_->emit((uint8_t)(idx & 0xFF), line);
            return;
        }
        case ExprKind::FloatLit: {
            auto* n = static_cast<const FloatLitExpr*>(e);
            int idx = chunk_->addConstant(Value(n->value));
            chunk_->emitOp(OpCode::CONST, line);
            chunk_->emit((uint8_t)((idx >> 8) & 0xFF), line);
            chunk_->emit((uint8_t)(idx & 0xFF), line);
            return;
        }
        case ExprKind::StringLit: {
            auto* n = static_cast<const StringLitExpr*>(e);
            int idx = chunk_->addConstant(Value(n->value));
            chunk_->emitOp(OpCode::CONST, line);
            chunk_->emit((uint8_t)((idx >> 8) & 0xFF), line);
            chunk_->emit((uint8_t)(idx & 0xFF), line);
            return;
        }
        case ExprKind::BoolLit: {
            auto* n = static_cast<const BoolLitExpr*>(e);
            chunk_->emitOp(n->value ? OpCode::TRUE_V : OpCode::FALSE_V, line);
            return;
        }
        case ExprKind::NoneLit:
            chunk_->emitOp(OpCode::NONE, line);
            return;

        case ExprKind::NameRef: {
            auto* n = static_cast<const NameRefExpr*>(e);
            emitNameU16(OpCode::LOAD, n->name, line);
            return;
        }

        case ExprKind::Grouping:
            compileExpr(static_cast<const GroupingExpr*>(e)->inner.get());
            return;

        case ExprKind::Unary: {
            auto* n = static_cast<const UnaryExpr*>(e);
            compileExpr(n->operand.get());
            switch (n->op) {
            case UnOp::Neg:  chunk_->emitOp(OpCode::NEG, line);  break;
            case UnOp::Pos:  break;
            case UnOp::Not:  chunk_->emitOp(OpCode::NOT, line);  break;
            case UnOp::BNot: chunk_->emitOp(OpCode::BNOT, line); break;
            }
            return;
        }

        case ExprKind::Binary: {
            auto* n = static_cast<const BinaryExpr*>(e);

            if (n->op == BinOp::And) {
                compileExpr(n->lhs.get());
                chunk_->emitOp(OpCode::DUP, line);
                size_t j = emitJump(OpCode::JUMP_IF_FALSE, line);
                chunk_->emitOp(OpCode::POP, line);
                compileExpr(n->rhs.get());
                patchJump(j, chunk_->here());
                return;
            }
            if (n->op == BinOp::Or) {
                compileExpr(n->lhs.get());
                chunk_->emitOp(OpCode::DUP, line);
                size_t j = emitJump(OpCode::JUMP_IF_TRUE, line);
                chunk_->emitOp(OpCode::POP, line);
                compileExpr(n->rhs.get());
                patchJump(j, chunk_->here());
                return;
            }

            compileExpr(n->lhs.get());
            compileExpr(n->rhs.get());
            switch (n->op) {
            case BinOp::Add:      chunk_->emitOp(OpCode::ADD, line); break;
            case BinOp::Sub:      chunk_->emitOp(OpCode::SUB, line); break;
            case BinOp::Mul:      chunk_->emitOp(OpCode::MUL, line); break;
            case BinOp::Div:      chunk_->emitOp(OpCode::DIV, line); break;
            case BinOp::FloorDiv: chunk_->emitOp(OpCode::FLOORDIV, line); break;
            case BinOp::Mod:      chunk_->emitOp(OpCode::MOD, line); break;
            case BinOp::Pow:      chunk_->emitOp(OpCode::POW, line); break;
            case BinOp::Eq:       chunk_->emitOp(OpCode::EQ, line); break;
            case BinOp::NotEq:    chunk_->emitOp(OpCode::NEQ, line); break;
            case BinOp::Lt:       chunk_->emitOp(OpCode::LT, line); break;
            case BinOp::Gt:       chunk_->emitOp(OpCode::GT, line); break;
            case BinOp::LtEq:     chunk_->emitOp(OpCode::LE, line); break;
            case BinOp::GtEq:     chunk_->emitOp(OpCode::GE, line); break;
            case BinOp::In:       chunk_->emitOp(OpCode::IN, line); break;
            case BinOp::BAnd:     chunk_->emitOp(OpCode::BAND, line); break;
            case BinOp::BOr:      chunk_->emitOp(OpCode::BOR, line); break;
            case BinOp::BXor:     chunk_->emitOp(OpCode::BXOR, line); break;
            case BinOp::Shl:      chunk_->emitOp(OpCode::SHL, line); break;
            case BinOp::Shr:      chunk_->emitOp(OpCode::SHR, line); break;
            case BinOp::Is:
                error(e->loc, "VM mode: operator 'is' not yet implemented");
            case BinOp::And:
            case BinOp::Or:
                return;
            }
            return;
        }

        case ExprKind::Call:
            compileCall(static_cast<const CallExpr*>(e), line);
            return;

        case ExprKind::Attr: {
            auto* a = static_cast<const AttrExpr*>(e);

            if (a->target->kind == ExprKind::NameRef) {
                const auto* tn = static_cast<const NameRefExpr*>(a->target.get());

                // Phase 11.1d: enum item access -> integer constant.
                auto eit = enums_.find(tn->name);
                if (eit != enums_.end()) {
                    auto iit = eit->second.find(a->name);
                    if (iit == eit->second.end())
                        error(a->loc, "enum '" + tn->name + "' has no item '" +
                            a->name + "'");
                    int idx = chunk_->addConstant(Value(iit->second));
                    chunk_->emitOp(OpCode::CONST, line);
                    chunk_->emit((uint8_t)((idx >> 8) & 0xFF), line);
                    chunk_->emit((uint8_t)(idx & 0xFF), line);
                    return;
                }

                // Phase 11.1c: ClassName.staticName -> LOAD mangled global.
                auto sit = statics_.find(tn->name);
                if (sit != statics_.end() && sit->second.count(a->name)) {
                    emitNameU16(OpCode::LOAD, staticName(tn->name, a->name), line);
                    return;
                }
            }

            compileAttrGet(a, line);
            return;
        }

        case ExprKind::Index: {
            auto* n = static_cast<const IndexExpr*>(e);
            compileExpr(n->target.get());
            compileExpr(n->index.get());
            chunk_->emitOp(OpCode::INDEX_GET, line);
            return;
        }

        case ExprKind::ListLit: {
            auto* n = static_cast<const ListLitExpr*>(e);
            for (auto& el : n->elements) compileExpr(el.get());
            chunk_->emitOp(OpCode::LIST_NEW, line);
            int cnt = (int)n->elements.size();
            chunk_->emit((uint8_t)((cnt >> 8) & 0xFF), line);
            chunk_->emit((uint8_t)(cnt & 0xFF), line);
            return;
        }

        case ExprKind::MapLit: {
            auto* n = static_cast<const MapLitExpr*>(e);
            for (auto& entry : n->entries) {
                compileExpr(entry.key.get());
                compileExpr(entry.value.get());
            }
            chunk_->emitOp(OpCode::MAP_NEW, line);
            int cnt = (int)n->entries.size();
            chunk_->emit((uint8_t)((cnt >> 8) & 0xFF), line);
            chunk_->emit((uint8_t)(cnt & 0xFF), line);
            return;
        }

        case ExprKind::Lambda:
            compileLambda(static_cast<const LambdaExpr*>(e));
            return;

        case ExprKind::GenericType:
            error(e->loc, "VM mode: generic type expression used as value");
        }
    }

} // namespace vayu