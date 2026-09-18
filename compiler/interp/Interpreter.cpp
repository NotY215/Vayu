#include "Interpreter.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace vayu {

    Interpreter* Interpreter::current_ = nullptr;

    struct ReturnSignal { Value value; };
    struct BreakSignal {};
    struct ContinueSignal {};

    namespace {
        struct EnvGuard {
            std::shared_ptr<Environment>& slot;
            std::shared_ptr<Environment>  saved;
            EnvGuard(std::shared_ptr<Environment>& s, std::shared_ptr<Environment> n)
                : slot(s), saved(std::move(s)) {
                slot = std::move(n);
            }
            ~EnvGuard() { slot = std::move(saved); }
        };
        struct ActiveExcGuard {
            std::vector<Value>& vec;
            ActiveExcGuard(std::vector<Value>& v, Value e) : vec(v) {
                vec.push_back(std::move(e));
            }
            ~ActiveExcGuard() { vec.pop_back(); }
        };
        bool valueEquals(const Value& a, const Value& b) {
            if (a.isNumber() && b.isNumber()) return a.asDouble() == b.asDouble();
            if (a.isString() && b.isString()) return a.asString() == b.asString();
            if (a.isBool() && b.isBool())   return a.asBool() == b.asBool();
            if (a.isNone() && b.isNone())   return true;
            if (a.isList() && b.isList())   return a.asList() == b.asList();
            if (a.isMap() && b.isMap())    return a.asMap() == b.asMap();
            if (a.isInstance() && b.isInstance()) return a.asInstance() == b.asInstance();
            return false;
        }
    } // namespace

    std::string staticGlobalName(const std::string& cls, const std::string& m) {
        return "__static_" + cls + "__" + m;
    }

    Interpreter::Interpreter() {
        current_ = this;
        globals_ = std::make_shared<Environment>(nullptr);
        env_ = globals_;
        installExceptionClasses();
        installBuiltins();
        installMathModule();
    }

    void Interpreter::run(const Block& program) {
        for (auto& s : program.stmts) {
            if (s->kind == StmtKind::Struct) registerStruct(static_cast<const StructStmt*>(s.get()));
            if (s->kind == StmtKind::Class)  registerClass(static_cast<const ClassStmt*>(s.get()));
            if (s->kind == StmtKind::Enum)   registerEnum(static_cast<const EnumStmt*>(s.get()));
        }
        execBlock(program);
    }

    bool Interpreter::vmIsInstanceOf(const Value& v, const std::string& className) {
        auto it = classes_.find(className);
        if (it == classes_.end()) return false;
        return valueIsInstanceOf(v, it->second);
    }

    Value Interpreter::vmMakeException(const std::string& typeName,
        const std::string& msg) {
        return makeException(typeName, msg);
    }

    void Interpreter::registerDeclarations(const Block& program) {
        for (auto& s : program.stmts) {
            if (s->kind == StmtKind::Struct)
                registerStruct(static_cast<const StructStmt*>(s.get()));
            if (s->kind == StmtKind::Class)
                registerClass(static_cast<const ClassStmt*>(s.get()));
            if (s->kind == StmtKind::Enum)
                registerEnum(static_cast<const EnumStmt*>(s.get()));
        }
        // Phase 11.1c: the VM never runs the top-level Class statement, so
        // static initializers must run here.  Statics live in globals_ under
        // the same mangled name the VM compiler emits via DEFINE, so both
        // storage paths agree.
        for (auto& s : program.stmts) {
            if (s->kind != StmtKind::Class) continue;
            auto* n = static_cast<const ClassStmt*>(s.get());
            for (auto& sf : n->staticFields) {
                Value v = sf.init ? eval(sf.init.get()) : Value();
                globals_->define(staticGlobalName(n->name, sf.name), std::move(v));
            }
        }
    }

    Value Interpreter::vmGetAttr(const Value& base, const std::string& name,
        SourceLocation loc) {
        if (base.isCallable() &&
            base.asCallable()->kind == Callable::Kind::SuperMethod) {
            auto sp = base.asCallable();
            std::shared_ptr<ClassObject> dummy;
            auto m = findMethod(sp->superParent, name, &dummy);
            if (!m)
                throw RuntimeError("parent class has no method '" + name + "'", loc);
            auto bm = std::make_shared<Callable>();
            bm->kind = Callable::Kind::BoundMethod;
            bm->name = name;
            bm->boundSelf = sp->boundSelf;
            bm->methodFn = m;
            bm->definingClass = dummy;
            return Value(bm);
        }
        if (base.isCallable() &&
            base.asCallable()->kind == Callable::Kind::ClassCtor) {
            auto cls = base.asCallable()->classObj;
            if (cls) {
                const std::string mangled = staticGlobalName(cls->name, name);
                if (Value* slot = globals_->lookup(mangled)) return *slot;
            }
        }
        if (base.isModule()) {
            auto mod = base.asModule();
            auto it = mod->members.find(name);
            if (it == mod->members.end())
                throw RuntimeError("module '" + mod->name + "' has no member '" +
                    name + "'", loc);
            return it->second;
        }
        if (base.isString()) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::StringMethod;
            c->name = name;
            c->boundStr = base.asString();
            return Value(c);
        }
        if (base.isList()) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::ListMethod;
            c->name = name;
            c->boundList = base.asList();
            return Value(c);
        }
        if (base.isMap()) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::MapMethod;
            c->name = name;
            c->boundMap = base.asMap();
            return Value(c);
        }
        if (!base.isInstance())
            throw RuntimeError("cannot read '" + name + "' on value of type " +
                base.typeName(), loc);
        auto si = base.asInstance();
        auto fit = si->fields.find(name);
        if (fit != si->fields.end()) return fit->second;
        if (si->cls) {
            std::shared_ptr<ClassObject> defCls;
            auto m = findMethod(si->cls, name, &defCls);
            if (m) {
                auto bm = std::make_shared<Callable>();
                bm->kind = Callable::Kind::BoundMethod;
                bm->name = name;
                bm->boundSelf = si;
                bm->methodFn = m;
                bm->definingClass = defCls;
                return Value(bm);
            }
        }
        throw RuntimeError("type '" + (si->cls ? si->cls->name : "?") +
            "' has no field or method '" + name + "'", loc);
    }

    void Interpreter::vmSetAttr(const Value& base, const std::string& name,
        const Value& v, SourceLocation loc) {
        // Phase 11.1c: ClassName.staticName = value
        if (base.isCallable() &&
            base.asCallable()->kind == Callable::Kind::ClassCtor) {
            auto cls = base.asCallable()->classObj;
            if (cls) {
                globals_->define(staticGlobalName(cls->name, name), v);
                return;
            }
        }
        if (!base.isInstance())
            throw RuntimeError("cannot set field '" + name + "' on value of type " +
                base.typeName(), loc);
        base.asInstance()->fields[name] = v;
    }

    Value Interpreter::vmNewInst(const std::string& className,
        const std::vector<Value>& args,
        SourceLocation loc) {
        auto it = classes_.find(className);
        if (it == classes_.end())
            throw RuntimeError("unknown struct or class '" + className + "'", loc);
        std::vector<std::pair<std::string, Value>> kwargs;
        kwargs.reserve(args.size());
        for (auto& a : args) kwargs.emplace_back("", a);
        return constructInstance(it->second, kwargs, loc);
    }

    void Interpreter::registerStruct(const StructStmt* d) {
        auto c = std::make_shared<ClassObject>();
        c->name = d->name;
        for (auto& f : d->fields) c->fieldOrder.push_back(f.name);
        classes_[d->name] = c;
    }
    void Interpreter::registerClass(const ClassStmt* d) {
        auto c = std::make_shared<ClassObject>();
        c->name = d->name;
        for (auto& f : d->fields) c->fieldOrder.push_back(f.name);
        if (!d->parentName.empty()) {
            auto p = classes_.find(d->parentName);
            if (p == classes_.end())
                throw RuntimeError("unknown parent class '" + d->parentName + "'", d->loc);
            c->parent = p->second;
        }
        classes_[d->name] = c;
        classDecls_[d->name] = d;
    }

    // Phase 11.1d — enums become module-shaped values in globals_.
    void Interpreter::registerEnum(const EnumStmt* d) {
        auto mod = std::make_shared<ModuleValue>();
        mod->name = d->name;
        for (auto& it : d->items) {
            long long v = 0;
            if (it.value && it.value->kind == ExprKind::IntLit)
                v = static_cast<const IntLitExpr*>(it.value.get())->value;
            mod->members[it.name] = Value(v);
        }
        globals_->define(d->name, Value(mod));
    }

    // ===========================================================================
    // Exceptions
    // ===========================================================================

    void Interpreter::installExceptionClasses() {
        auto base = std::make_shared<ClassObject>();
        base->name = "Exception";
        base->fieldOrder = { "message" };
        classes_["Exception"] = base;
        auto mkClass = [&](const std::string& name, std::shared_ptr<ClassObject> parent) {
            auto c = std::make_shared<ClassObject>();
            c->name = name; c->parent = std::move(parent);
            classes_[name] = c;
            return c;
            };
        mkClass("ValueError", base);
        mkClass("TypeError", base);
        auto rt = mkClass("RuntimeError", base);
        mkClass("ZeroDivisionError", rt);
        mkClass("IndexError", base);
        mkClass("KeyError", base);
        mkClass("NameError", base);
        mkClass("AttributeError", base);
    }

    bool Interpreter::valueIsInstanceOf(const Value& v,
        const std::shared_ptr<ClassObject>& cls) const {
        if (!v.isInstance()) return false;
        for (auto c = v.asInstance()->cls; c; c = c->parent)
            if (c == cls || c->name == cls->name) return true;
        return false;
    }

    Value Interpreter::makeException(const std::string& typeName, const std::string& msg) {
        auto it = classes_.find(typeName);
        if (it == classes_.end()) it = classes_.find("Exception");
        if (it == classes_.end()) return Value(msg);
        auto si = std::make_shared<StructInstance>();
        si->cls = it->second;
        si->fields["message"] = Value(msg);
        return Value(si);
    }

    std::string Interpreter::exceptionTypeName(const Value& v) {
        if (v.isInstance() && v.asInstance()->cls) return v.asInstance()->cls->name;
        return "Exception";
    }
    std::string Interpreter::exceptionMessage(const Value& v) {
        if (v.isInstance()) {
            auto it = v.asInstance()->fields.find("message");
            if (it != v.asInstance()->fields.end() && it->second.isString())
                return it->second.asString();
        }
        return v.toString();
    }

    // ===========================================================================
    // Statement execution
    // ===========================================================================

    void Interpreter::execBlock(const Block& b) {
        for (auto& s : b.stmts) exec(s.get());
    }

    void Interpreter::exec(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
        case StmtKind::Expr: eval(static_cast<const ExprStmt*>(s)->expr.get()); return;
        case StmtKind::Assign: execAssign(static_cast<const AssignStmt*>(s)); return;
        case StmtKind::AnnotAssign: {
            auto* n = static_cast<const AnnotAssignStmt*>(s);
            if (n->value) env_->define(n->name, eval(n->value.get()));
            else          env_->define(n->name, Value());
            return;
        }
        case StmtKind::Const: {
            auto* n = static_cast<const ConstStmt*>(s);
            env_->define(n->name, eval(n->value.get()));
            return;
        }
        case StmtKind::If: {
            auto* n = static_cast<const IfStmt*>(s);
            if (eval(n->cond.get()).truthy()) { execBlock(n->thenBody); return; }
            for (auto& ec : n->elifs) {
                if (eval(ec.cond.get()).truthy()) { execBlock(ec.body); return; }
            }
            if (n->elseBody) execBlock(*n->elseBody);
            return;
        }
        case StmtKind::While: {
            auto* n = static_cast<const WhileStmt*>(s);
            while (eval(n->cond.get()).truthy()) {
                try { execBlock(n->body); }
                catch (BreakSignal&) { break; }
                catch (ContinueSignal&) { continue; }
            }
            return;
        }
        case StmtKind::For:   execFor(static_cast<const ForStmt*>(s));  return;
        case StmtKind::Try:   execTry(static_cast<const TryStmt*>(s));  return;
        case StmtKind::Raise: execRaise(static_cast<const RaiseStmt*>(s));return;
        case StmtKind::Yield: execYield(static_cast<const YieldStmt*>(s));return;
        case StmtKind::Import:     execImport(static_cast<const ImportStmt*>(s));     return;
        case StmtKind::FromImport: execFromImport(static_cast<const FromImportStmt*>(s)); return;
        case StmtKind::Def: {
            auto* n = static_cast<const DefStmt*>(s);
            auto fn = std::make_shared<Callable>();
            fn->kind = Callable::Kind::User;
            fn->name = n->name;
            fn->decl = n;
            fn->closure = env_;
            env_->define(n->name, Value(fn));
            return;
        }
        case StmtKind::Return: {
            auto* n = static_cast<const ReturnStmt*>(s);
            ReturnSignal sig;
            if (n->value) sig.value = eval(n->value.get());
            throw sig;
        }

        case StmtKind::Class: {
            // Phase 11.1c: statics live in globals_ under a mangled name that
            // matches what the VM compiler emits via DEFINE.
            auto* n = static_cast<const ClassStmt*>(s);
            for (auto& sf : n->staticFields) {
                Value v = sf.init ? eval(sf.init.get()) : Value();
                globals_->define(staticGlobalName(n->name, sf.name), std::move(v));
            }
            return;
        }

        case StmtKind::Struct:
        case StmtKind::Enum: return;
        case StmtKind::Pass:     return;
        case StmtKind::Break:    throw BreakSignal{};
        case StmtKind::Continue: throw ContinueSignal{};
        }
    }
    // ===========================================================================
// Phase 11.1k1 — generators
// ===========================================================================

    void Interpreter::execYield(const YieldStmt* y) {
        if (!generatorContext_)
            throw RuntimeError("'yield' outside a generator function", y->loc);
        Value v = y->value ? eval(y->value.get()) : Value();
        auto gen = generatorContext_;

        // Save our generator-local env so we can restore it after the wait.
        auto myEnv = env_;
        auto myGen = generatorContext_;

        std::unique_lock<std::mutex> lk(gen->mtx);
        gen->yielded = std::move(v);
        gen->state = GenState::Suspended;

        // Hand the interpreter's env back to the owner thread while we wait.
        env_ = gen->ownerEnv;
        generatorContext_ = gen->ownerGen;

        gen->cv.notify_all();
        gen->cv.wait(lk, [&] { return gen->resume || gen->cancel; });

        // We're back — restore our generator-local env.
        env_ = myEnv;
        generatorContext_ = myGen;

        if (gen->cancel) throw GeneratorCancelled{};
        gen->resume = false;
        gen->state = GenState::Running;
    }

    Value Interpreter::createGenerator(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args) {
        const DefStmt* d = fn->decl;
        if (args.size() != d->params.size())
            throw RuntimeError("function '" + fn->name + "' expects " +
                std::to_string(d->params.size()) + " argument(s), got " +
                std::to_string(args.size()), SourceLocation{});

        auto gen = std::make_shared<GeneratorValue>();
        auto callEnv = std::make_shared<Environment>(
            fn->closure ? fn->closure : globals_);
        for (size_t i = 0; i < args.size(); ++i)
            callEnv->define(d->params[i].name, args[i]);
        if (fn->definingClass)
            callEnv->define("__class__", Value(fn->definingClass));

        Interpreter* self = this;
        gen->worker = std::thread([self, gen, callEnv, d]() {
            Interpreter::current_ = self;

            // Wait for the first resume() call from the owner.
            {
                std::unique_lock<std::mutex> lk(gen->mtx);
                gen->cv.wait(lk, [&] { return gen->resume || gen->cancel; });
                if (gen->cancel) {
                    gen->state = GenState::Done;
                    gen->cv.notify_all();
                    return;
                }
                gen->resume = false;
            }

            // The owner is blocked on gen->cv; we are the only thread
            // touching the Interpreter.  Capture the owner's state and
            // install our own.
            auto outerEnv = self->env_;
            auto outerGen = self->generatorContext_;
            self->env_ = callEnv;
            self->generatorContext_ = gen;

            try {
                try {
                    for (auto& st : d->body.stmts) self->exec(st.get());
                }
                catch (ReturnSignal&) {
                    // Normal generator exit.
                }
            }
            catch (GeneratorCancelled&) {
                // Owner cancelled — drop out.
            }
            catch (...) {
                std::lock_guard<std::mutex> lk(gen->mtx);
                gen->pendingError = std::current_exception();
            }

            // Always restore the owner's env before publishing Done.
            self->env_ = outerEnv;
            self->generatorContext_ = outerGen;

            std::lock_guard<std::mutex> lk(gen->mtx);
            gen->state = GenState::Done;
            gen->cv.notify_all();
            });

        return Value(gen);
    }

    Value Interpreter::nextGenerator(const std::shared_ptr<GeneratorValue>& gen,
        SourceLocation loc) {
        std::unique_lock<std::mutex> lk(gen->mtx);
        if (gen->state == GenState::Done)
            throw RuntimeError("generator exhausted", loc);
        if (gen->state == GenState::Running)
            throw RuntimeError("generator already running", loc);

        // Hand our env to the generator so it can restore it while suspended.
        gen->ownerEnv = env_;
        gen->ownerGen = generatorContext_;

        gen->resume = true;
        gen->state = GenState::Running;
        gen->cv.notify_all();
        gen->cv.wait(lk, [&] {
            return gen->state == GenState::Suspended ||
                gen->state == GenState::Done;
            });

        if (gen->state == GenState::Done) {
            if (gen->pendingError) {
                auto err = gen->pendingError;
                gen->pendingError = nullptr;
                lk.unlock();
                std::rethrow_exception(err);
            }
            throw RuntimeError("generator exhausted", loc);
        }
        Value v = std::move(gen->yielded);
        return v;
    }

    bool Interpreter::tryNextGenerator(const std::shared_ptr<GeneratorValue>& gen,
        Value& out, SourceLocation loc) {
        std::unique_lock<std::mutex> lk(gen->mtx);
        if (gen->state == GenState::Done) {
            if (gen->pendingError) {
                auto err = gen->pendingError;
                gen->pendingError = nullptr;
                lk.unlock();
                std::rethrow_exception(err);
            }
            return false;
        }
        if (gen->state == GenState::Running) {
            lk.unlock();
            throw RuntimeError("generator already running", loc);
        }
        gen->ownerEnv = env_;
        gen->ownerGen = generatorContext_;
        gen->resume = true;
        gen->state = GenState::Running;
        gen->cv.notify_all();
        gen->cv.wait(lk, [&] {
            return gen->state == GenState::Suspended ||
                gen->state == GenState::Done;
            });
        if (gen->state == GenState::Done) {
            if (gen->pendingError) {
                auto err = gen->pendingError;
                gen->pendingError = nullptr;
                lk.unlock();
                std::rethrow_exception(err);
            }
            return false;
        }
        out = std::move(gen->yielded);
        return true;
    }

    void Interpreter::execRaise(const RaiseStmt* r) {
        Value v;
        if (r->exception) v = eval(r->exception.get());
        else {
            if (activeExceptions_.empty())
                throw RuntimeError("no active exception to re-raise", r->loc);
            v = activeExceptions_.back();
        }
        if (!v.isInstance()) v = makeException("Exception", v.toString());
        throw VayuException{ v, r->loc };
    }

    // ===========================================================================
    // Module loading
    // ===========================================================================

    bool Interpreter::findModuleFile(const std::string& name, std::string& pathOut) const {
        auto tryPath = [&](const std::string& p) -> bool {
            std::ifstream in(p, std::ios::binary);
            if (in) { pathOut = p; return true; }
            return false;
            };

        if (!sourceDir_.empty()) {
            std::string p = sourceDir_;
            if (p.back() != '/' && p.back() != '\\') p += '/';
            p += name + ".vayu";
            if (tryPath(p)) return true;
            std::string p2 = sourceDir_;
            if (p2.back() != '/' && p2.back() != '\\') p2 += '/';
            p2 += name + ".vyu";
            if (tryPath(p2)) return true;
        }
        if (tryPath(name + ".vayu")) return true;
        if (tryPath(name + ".vyu"))  return true;

        if (const char* mp = std::getenv("VAYU_MODULE_PATH")) {
#ifdef _WIN32
            const char sep = ';';
#else
            const char sep = ':';
#endif
            std::string s = mp;
            size_t start = 0;
            while (start <= s.size()) {
                size_t end = s.find(sep, start);
                if (end == std::string::npos) end = s.size();
                std::string dir = s.substr(start, end - start);
                if (!dir.empty()) {
                    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
                    if (tryPath(dir + name + ".vayu")) return true;
                    if (tryPath(dir + name + ".vyu"))  return true;
                }
                if (end == s.size()) break;
                start = end + 1;
            }
        }
        return false;
    }

    Value Interpreter::loadModule(const std::string& name, SourceLocation loc) {
        auto cached = moduleCache_.find(name);
        if (cached != moduleCache_.end()) return Value(cached->second);

        if (Value* existing = globals_->lookup(name)) {
            if (existing->isModule()) {
                moduleCache_[name] = existing->asModule();
                return *existing;
            }
        }

        std::string path;
        if (!findModuleFile(name, path))
            throw RuntimeError("cannot find module '" + name + "' (looked for '" +
                name + ".vayu')", loc);

        std::ifstream in(path, std::ios::binary);
        if (!in) throw RuntimeError("cannot open module file '" + path + "'", loc);
        std::stringstream ss; ss << in.rdbuf();
        std::string src = ss.str();

        Block program;
        try {
            Lexer lexer(std::move(src));
            auto tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            program = parser.parseProgram();
        }
        catch (const ParseError& e) {
            throw RuntimeError("parse error in module '" + name + "' at line " +
                std::to_string(e.loc.line) + ": " + e.what(), loc);
        }

        moduleAsts_.push_back(std::make_unique<Block>(std::move(program)));
        Block& programRef = *moduleAsts_.back();

        auto modEnv = std::make_shared<Environment>(globals_);

        std::string moduleDir;
        {
            auto slash = path.find_last_of("/\\");
            moduleDir = (slash == std::string::npos) ? std::string{} : path.substr(0, slash + 1);
        }
        std::string savedDir = sourceDir_;
        sourceDir_ = moduleDir;

        auto savedEnv = env_;
        env_ = modEnv;
        try {
            for (auto& s2 : programRef.stmts) {
                if (s2->kind == StmtKind::Struct)
                    registerStruct(static_cast<const StructStmt*>(s2.get()));
                if (s2->kind == StmtKind::Class)
                    registerClass(static_cast<const ClassStmt*>(s2.get()));
                if (s2->kind == StmtKind::Enum)
                    registerEnum(static_cast<const EnumStmt*>(s2.get()));
            }
            execBlock(programRef);
        }
        catch (...) {
            env_ = savedEnv;
            sourceDir_ = savedDir;
            throw;
        }
        env_ = savedEnv;
        sourceDir_ = savedDir;

        auto mod = std::make_shared<ModuleValue>();
        mod->name = name;
        for (auto& [k, v] : modEnv->localVars()) {
            mod->members[k] = v;
        }

        moduleCache_[name] = mod;
        return Value(mod);
    }

    void Interpreter::execImport(const ImportStmt* n) {
        Value mod = loadModule(n->moduleName, n->loc);
        const std::string& bind = n->alias.empty() ? n->moduleName : n->alias;
        env_->define(bind, mod);
    }

    void Interpreter::execFromImport(const FromImportStmt* n) {
        Value mod = loadModule(n->moduleName, n->loc);
        if (!mod.isModule())
            throw RuntimeError("'" + n->moduleName + "' is not a module", n->loc);
        auto mv = mod.asModule();
        for (auto& item : n->items) {
            auto it = mv->members.find(item.name);
            if (it == mv->members.end())
                throw RuntimeError("module '" + n->moduleName +
                    "' has no member '" + item.name + "'", n->loc);
            const std::string& bind = item.alias.empty() ? item.name : item.alias;
            env_->define(bind, it->second);
        }
    }

    void Interpreter::execTry(const TryStmt* t) {
        bool finallyRan = false;
        auto runFinally = [&]() {
            if (finallyRan) return;
            finallyRan = true;
            if (t->finallyBody) execBlock(*t->finallyBody);
            };
        auto dispatch = [&](const VayuException& ne) -> bool {
            for (auto& h : t->handlers) {
                bool matches = false;
                if (!h.exceptionType) matches = true;
                else {
                    Value expected = eval(h.exceptionType.get());
                    std::shared_ptr<ClassObject> cls;
                    if (expected.isClass()) cls = expected.asClass();
                    else if (expected.isCallable() &&
                        expected.asCallable()->kind == Callable::Kind::ClassCtor)
                        cls = expected.asCallable()->classObj;
                    else throw RuntimeError("except type must be a class",
                        h.exceptionType->loc);
                    matches = valueIsInstanceOf(ne.value, cls);
                }
                if (matches) {
                    if (!h.varName.empty()) env_->define(h.varName, ne.value);
                    ActiveExcGuard g(activeExceptions_, ne.value);
                    execBlock(h.body);
                    return true;
                }
            }
            return false;
            };
        try { execBlock(t->tryBody); }
        catch (ReturnSignal&) { runFinally(); throw; }
        catch (BreakSignal&) { runFinally(); throw; }
        catch (ContinueSignal&) { runFinally(); throw; }
        catch (VayuException& ne) {
            bool handled = false;
            try { handled = dispatch(ne); }
            catch (...) { runFinally(); throw; }
            if (!handled) { runFinally(); throw; }
        }
        catch (RuntimeError& e) {
            VayuException ne{ makeException("RuntimeError", e.what()), e.loc };
            bool handled = false;
            try { handled = dispatch(ne); }
            catch (...) { runFinally(); throw; }
            if (!handled) { runFinally(); throw ne; }
        }
        runFinally();
    }

    void Interpreter::execAssign(const AssignStmt* n) {
        Value v = eval(n->value.get());
        if (n->target->kind == ExprKind::NameRef) {
            const auto* nm = static_cast<const NameRefExpr*>(n->target.get());
            if (!env_->assign(nm->name, v)) env_->define(nm->name, std::move(v));
            return;
        }
        if (n->target->kind == ExprKind::Attr) {
            auto* a = static_cast<const AttrExpr*>(n->target.get());
            Value inst = eval(a->target.get());
            // Phase 11.1c: ClassName.staticName = value
            if (inst.isCallable() &&
                inst.asCallable()->kind == Callable::Kind::ClassCtor) {
                auto cls = inst.asCallable()->classObj;
                if (cls) {
                    globals_->define(staticGlobalName(cls->name, a->name),
                        std::move(v));
                    return;
                }
            }
            if (!inst.isInstance())
                throw RuntimeError("cannot assign field on " + inst.typeName(), a->loc);
            inst.asInstance()->fields[a->name] = std::move(v);
            return;
        }
        if (n->target->kind == ExprKind::Index) {
            auto* ix = static_cast<const IndexExpr*>(n->target.get());
            Value tgt = eval(ix->target.get());
            Value idx = eval(ix->index.get());
            if (tgt.isList()) {
                if (!idx.isInt())
                    throw RuntimeError("list index must be int, got " + idx.typeName(), ix->loc);
                auto lst = tgt.asList();
                long long i = idx.asInt();
                if (i < 0) i += (long long)lst->items.size();
                if (i < 0 || i >= (long long)lst->items.size())
                    throw RuntimeError("list index out of range", ix->loc);
                lst->items[(size_t)i] = std::move(v);
                return;
            }
            if (tgt.isMap()) {
                if (!idx.isString())
                    throw RuntimeError("map key must be str, got " + idx.typeName(), ix->loc);
                tgt.asMap()->entries[idx.asString()] = std::move(v);
                return;
            }
            if (tgt.isString())
                throw RuntimeError("strings are immutable", ix->loc);
            throw RuntimeError("cannot index-assign to value of type " + tgt.typeName(), ix->loc);
        }
        throw RuntimeError("invalid assignment target", n->target->loc);
    }

    void Interpreter::execFor(const ForStmt* n) {
        Value iterable = eval(n->iterable.get());
        auto bindVar = [&](Value v) {
            if (!env_->assign(n->targetName, v)) env_->define(n->targetName, std::move(v));
            };
        auto runBody = [&]() -> bool {
            try { execBlock(n->body); }
            catch (BreakSignal&) { return false; }
            catch (ContinueSignal&) { return true; }
            return true;
            };
        if (iterable.isList()) {
            auto lst = iterable.asList();
            size_t count = lst->items.size();
            for (size_t i = 0; i < count; ++i) {
                if (i >= lst->items.size()) break;
                bindVar(lst->items[i]);
                if (!runBody()) break;
            }
            return;
        }
        if (iterable.isMap()) {
            auto m = iterable.asMap();
            std::vector<std::string> keys;
            keys.reserve(m->entries.size());
            for (auto& [k, _] : m->entries) keys.push_back(k);
            for (auto& k : keys) { bindVar(Value(k)); if (!runBody()) break; }
            return;
        }
        if (iterable.isString()) {
            const std::string& s = iterable.asString();
            for (size_t i = 0; i < s.size(); ++i) {
                bindVar(Value(std::string(1, s[i])));
                if (!runBody()) break;
            }
            return;
        }
        if (iterable.isGenerator()) {
            auto gen = iterable.asGenerator();
            for (;;) {
                Value v;
                try {
                    v = nextGenerator(gen, n->loc);
                }
                catch (const RuntimeError&) {
                    break;   // exhausted
                }
                bindVar(v);
                if (!runBody()) break;
            }
            return;
        }
        throw RuntimeError("cannot iterate over value of type " + iterable.typeName(), n->loc);
    }

    // ===========================================================================
    // Expressions
    // ===========================================================================

    Value Interpreter::eval(const Expr* e) {
        if (!e) return Value();
        switch (e->kind) {
        case ExprKind::IntLit:    return Value(static_cast<const IntLitExpr*>(e)->value);
        case ExprKind::FloatLit:  return Value(static_cast<const FloatLitExpr*>(e)->value);
        case ExprKind::StringLit: return Value(static_cast<const StringLitExpr*>(e)->value);
        case ExprKind::CharLit:   return Value(static_cast<const CharLitExpr*>(e)->value);
        case ExprKind::BoolLit:   return Value(static_cast<const BoolLitExpr*>(e)->value);
        case ExprKind::NoneLit:   return Value();

        case ExprKind::NameRef: {
            auto* n = static_cast<const NameRefExpr*>(e);
            if (Value* slot = env_->lookup(n->name)) return *slot;
            auto it = classes_.find(n->name);
            if (it != classes_.end()) {
                auto c = std::make_shared<Callable>();
                c->kind = Callable::Kind::ClassCtor;
                c->name = n->name;
                c->classObj = it->second;
                return Value(c);
            }
            throw RuntimeError("name '" + n->name + "' is not defined", n->loc);
        }
        case ExprKind::Grouping:
            return eval(static_cast<const GroupingExpr*>(e)->inner.get());
        case ExprKind::Unary: {
            auto* n = static_cast<const UnaryExpr*>(e);
            Value v = eval(n->operand.get());
            switch (n->op) {
            case UnOp::Not: return Value(!v.truthy());
            case UnOp::Neg:
                if (v.isInt())   return Value(-v.asInt());
                if (v.isFloat()) return Value(-v.asFloat());
                throw RuntimeError("cannot negate " + v.typeName(), n->loc);
            case UnOp::Pos:
                if (v.isNumber()) return v;
                throw RuntimeError("cannot apply unary '+' to " + v.typeName(), n->loc);
            case UnOp::BNot:
                if (v.isInt()) return Value(~v.asInt());
                throw RuntimeError("cannot apply '~' to " + v.typeName(), n->loc);
            }
            return Value();
        }
        case ExprKind::Binary: return evalBinary(static_cast<const BinaryExpr*>(e));
        case ExprKind::Call:   return evalCall(static_cast<const CallExpr*>(e));
        case ExprKind::Attr:   return evalAttr(static_cast<const AttrExpr*>(e));
        case ExprKind::Index:  return evalIndex(static_cast<const IndexExpr*>(e));
        case ExprKind::ListLit:return evalListLit(static_cast<const ListLitExpr*>(e));
        case ExprKind::MapLit: return evalMapLit(static_cast<const MapLitExpr*>(e));

        case ExprKind::Lambda: {
            auto* n = static_cast<const LambdaExpr*>(e);
            auto fn = std::make_shared<Callable>();
            fn->kind = Callable::Kind::Lambda;
            fn->name = "<lambda>";
            fn->lambdaExpr = n;
            fn->closure = env_;
            return Value(fn);
        }
        case ExprKind::GenericType:
            throw RuntimeError("generic type expression used as a value", e->loc);
        }
        return Value();
    }

    Value Interpreter::evalListLit(const ListLitExpr* n) {
        auto lst = std::make_shared<ListValue>();
        lst->items.reserve(n->elements.size());
        for (auto& el : n->elements) lst->items.push_back(eval(el.get()));
        return Value(lst);
    }
    Value Interpreter::evalMapLit(const MapLitExpr* n) {
        auto m = std::make_shared<MapValue>();
        for (auto& entry : n->entries) {
            Value k = eval(entry.key.get());
            if (!k.isString())
                throw RuntimeError("map keys must be str, got " + k.typeName(), entry.key->loc);
            m->entries[k.asString()] = eval(entry.value.get());
        }
        return Value(m);
    }
    Value Interpreter::evalIndex(const IndexExpr* ix) {
        Value tgt = eval(ix->target.get());
        Value idx = eval(ix->index.get());
        if (tgt.isList()) {
            if (!idx.isInt())
                throw RuntimeError("list index must be int, got " + idx.typeName(), ix->loc);
            auto lst = tgt.asList();
            long long i = idx.asInt();
            if (i < 0) i += (long long)lst->items.size();
            if (i < 0 || i >= (long long)lst->items.size())
                throw RuntimeError("list index out of range", ix->loc);
            return lst->items[(size_t)i];
        }
        if (tgt.isMap()) {
            if (!idx.isString())
                throw RuntimeError("map key must be str, got " + idx.typeName(), ix->loc);
            auto m = tgt.asMap();
            auto it = m->entries.find(idx.asString());
            if (it == m->entries.end())
                throw RuntimeError("map has no key '" + idx.asString() + "'", ix->loc);
            return it->second;
        }
        if (tgt.isString()) {
            if (!idx.isInt())
                throw RuntimeError("str index must be int, got " + idx.typeName(), ix->loc);
            const std::string& s = tgt.asString();
            long long i = idx.asInt();
            if (i < 0) i += (long long)s.size();
            if (i < 0 || i >= (long long)s.size())
                throw RuntimeError("string index out of range", ix->loc);
            return Value(std::string(1, s[(size_t)i]));
        }
        throw RuntimeError("cannot index value of type " + tgt.typeName(), ix->loc);
    }

    Value Interpreter::evalAttr(const AttrExpr* a) {
        Value base = eval(a->target.get());

        if (base.isCallable() &&
            base.asCallable()->kind == Callable::Kind::ClassCtor) {
            auto cls = base.asCallable()->classObj;
            if (cls) {
                const std::string mangled = staticGlobalName(cls->name, a->name);
                if (Value* slot = globals_->lookup(mangled)) return *slot;
            }
        }

        if (base.isCallable() && base.asCallable()->kind == Callable::Kind::SuperMethod) {
            auto sp = base.asCallable();
            std::shared_ptr<ClassObject> dummy;
            auto m = findMethod(sp->superParent, a->name, &dummy);
            if (!m) throw RuntimeError("parent class has no method '" + a->name + "'", a->loc);
            auto bm = std::make_shared<Callable>();
            bm->kind = Callable::Kind::BoundMethod; bm->name = a->name;
            bm->boundSelf = sp->boundSelf; bm->methodFn = m; bm->definingClass = dummy;
            return Value(bm);
        }
        if (base.isModule()) {
            auto mod = base.asModule();
            auto it = mod->members.find(a->name);
            if (it == mod->members.end())
                throw RuntimeError("module '" + mod->name + "' has no member '" +
                    a->name + "'", a->loc);
            return it->second;
        }
        if (base.isString()) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::StringMethod; c->name = a->name; c->boundStr = base.asString();
            return Value(c);
        }
        if (base.isList()) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::ListMethod; c->name = a->name; c->boundList = base.asList();
            return Value(c);
        }
        if (base.isMap()) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::MapMethod; c->name = a->name; c->boundMap = base.asMap();
            return Value(c);
        }
        if (!base.isInstance())
            throw RuntimeError("cannot read '" + a->name + "' on value of type " +
                base.typeName(), a->loc);
        auto si = base.asInstance();
        auto fit = si->fields.find(a->name);
        if (fit != si->fields.end()) return fit->second;
        if (si->cls) {
            std::shared_ptr<ClassObject> defCls;
            auto m = findMethod(si->cls, a->name, &defCls);
            if (m) {
                auto bm = std::make_shared<Callable>();
                bm->kind = Callable::Kind::BoundMethod; bm->name = a->name;
                bm->boundSelf = si; bm->methodFn = m; bm->definingClass = defCls;
                return Value(bm);
            }
        }
        throw RuntimeError("type '" + (si->cls ? si->cls->name : "?") +
            "' has no field or method '" + a->name + "'", a->loc);
    }

    std::shared_ptr<Callable> Interpreter::vmLookupClass(const std::string& name) {
        auto it = classes_.find(name);
        if (it == classes_.end()) return nullptr;
        auto c = std::make_shared<Callable>();
        c->kind = Callable::Kind::ClassCtor;
        c->name = name;
        c->classObj = it->second;
        return c;
    }

    Value Interpreter::vmLoadModule(const std::string& name, SourceLocation loc) {
        return loadModule(name, loc);
    }

    std::shared_ptr<Callable> Interpreter::findMethod(
        const std::shared_ptr<ClassObject>& cls,
        const std::string& name,
        std::shared_ptr<ClassObject>* definingClass)
    {
        for (auto c = cls; c; c = c->parent) {
            auto it = classDecls_.find(c->name);
            if (it == classDecls_.end()) continue;
            for (auto& m : it->second->methods) {
                if (m->name == name) {
                    auto fn = std::make_shared<Callable>();
                    fn->kind = Callable::Kind::User; fn->name = name; fn->decl = m.get();
                    if (definingClass) *definingClass = c;
                    return fn;
                }
            }
        }
        return nullptr;
    }

    Value Interpreter::evalCall(const CallExpr* c) {
        if (c->callee->kind == ExprKind::NameRef) {
            const auto* nm = static_cast<const NameRefExpr*>(c->callee.get());
            if (nm->name == "super") {
                if (!c->args.empty()) throw RuntimeError("super() takes no arguments", c->loc);
                Value* s = env_->lookup("self");
                Value* k = env_->lookup("__class__");
                if (!s || !k || !s->isInstance() || !k->isClass())
                    throw RuntimeError("super() outside method", c->loc);
                auto sup = std::make_shared<Callable>();
                sup->kind = Callable::Kind::SuperMethod; sup->name = "super";
                sup->boundSelf = s->asInstance(); sup->superParent = k->asClass()->parent;
                if (!sup->superParent)
                    throw RuntimeError("class '" + k->asClass()->name + "' has no parent", c->loc);
                return Value(sup);
            }
        }
        Value callee = eval(c->callee.get());
        if (callee.isCallable() && callee.asCallable()->kind == Callable::Kind::ClassCtor) {
            std::vector<std::pair<std::string, Value>> kwargs;
            for (auto& a : c->args) kwargs.emplace_back(a.name, eval(a.value.get()));
            return constructInstance(callee.asCallable()->classObj, kwargs, c->loc);
        }
        if (callee.isCallable() && callee.asCallable()->kind == Callable::Kind::SuperMethod)
            throw RuntimeError("cannot call super() directly", c->loc);
        std::vector<Value> args;
        args.reserve(c->args.size());
        for (auto& a : c->args) {
            if (!a.name.empty())
                throw RuntimeError("function does not accept keyword arguments", a.loc);
            args.push_back(eval(a.value.get()));
        }
        return callValue(callee, args, c->loc);
    }

    // ===========================================================================
    // Instance construction
    // ===========================================================================

    Value Interpreter::constructInstance(
        const std::shared_ptr<ClassObject>& cls,
        const std::vector<std::pair<std::string, Value>>& args,
        SourceLocation loc)
    {
        auto inst = std::make_shared<StructInstance>();
        inst->cls = cls;
        const DefStmt* initDecl = nullptr;
        std::shared_ptr<ClassObject> initOwner;
        for (auto c = cls; c; c = c->parent) {
            auto it = classDecls_.find(c->name);
            if (it == classDecls_.end()) continue;
            for (auto& m : it->second->methods) {
                if (m->name == "__init__") { initDecl = m.get(); initOwner = c; break; }
            }
            if (initDecl) break;
        }
        if (initDecl) {
            std::vector<Value> callArgs;
            callArgs.push_back(Value(inst));
            for (auto& [k, v] : args) {
                if (!k.empty())
                    throw RuntimeError("__init__ does not accept keyword arguments", loc);
                callArgs.push_back(v);
            }
            auto fn = std::make_shared<Callable>();
            fn->kind = Callable::Kind::User; fn->name = "__init__";
            fn->decl = initDecl; fn->closure = env_; fn->definingClass = initOwner;
            callUser(fn, callArgs, loc);
            return Value(inst);
        }
        std::vector<std::string> allFields;
        for (auto c = cls; c; c = c->parent)
            for (auto& f : c->fieldOrder) allFields.push_back(f);
        size_t positional = 0;
        for (auto& [k, v] : args) {
            if (k.empty()) {
                if (positional >= allFields.size())
                    throw RuntimeError("too many positional arguments", loc);
                inst->fields[allFields[positional]] = v;
                ++positional;
            }
            else {
                bool found = false;
                for (auto& f : allFields) if (f == k) { found = true; break; }
                if (!found)
                    throw RuntimeError("class '" + cls->name + "' has no field '" + k + "'", loc);
                inst->fields[k] = v;
            }
        }
        for (auto& f : allFields)
            if (!inst->fields.count(f))
                throw RuntimeError("class '" + cls->name +
                    "' is missing value for field '" + f + "'", loc);
        return Value(inst);
    }

    // ===========================================================================
    // Binary
    // ===========================================================================

    Value Interpreter::evalBinary(const BinaryExpr* b) {
        if (b->op == BinOp::And) { Value l = eval(b->lhs.get()); return l.truthy() ? eval(b->rhs.get()) : l; }
        if (b->op == BinOp::Or) { Value l = eval(b->lhs.get()); return l.truthy() ? l : eval(b->rhs.get()); }

        if (b->op == BinOp::In) {
            Value l = eval(b->lhs.get());
            Value r = eval(b->rhs.get());
            if (r.isList()) {
                for (auto& v : r.asList()->items)
                    if (valueEquals(l, v)) return Value(true);
                return Value(false);
            }
            if (r.isMap()) {
                if (!l.isString()) return Value(false);
                return Value(r.asMap()->entries.count(l.asString()) > 0);
            }
            if (r.isString() && l.isString())
                return Value(r.asString().find(l.asString()) != std::string::npos);
            throw RuntimeError("'in' requires a list, map, or str on the right", b->loc);
        }

        Value l = eval(b->lhs.get());
        Value r = eval(b->rhs.get());
        auto numFail = [&]() -> void {
            throw RuntimeError(std::string("cannot apply '") + binOpName(b->op) +
                "' to " + l.typeName() + " and " + r.typeName(), b->loc);
            };
        if (b->op == BinOp::Eq || b->op == BinOp::NotEq) {
            bool eq = valueEquals(l, r);
            return Value(b->op == BinOp::Eq ? eq : !eq);
        }
        if (b->op == BinOp::Lt || b->op == BinOp::Gt ||
            b->op == BinOp::LtEq || b->op == BinOp::GtEq) {
            bool res = false;
            if (l.isNumber() && r.isNumber()) {
                double a = l.asDouble(), c = r.asDouble();
                switch (b->op) {
                case BinOp::Lt: res = a < c; break; case BinOp::Gt: res = a > c; break;
                case BinOp::LtEq: res = a <= c; break; case BinOp::GtEq: res = a >= c; break;
                default: break;
                }
            }
            else if (l.isString() && r.isString()) {
                const auto& a = l.asString(); const auto& c = r.asString();
                switch (b->op) {
                case BinOp::Lt: res = a < c; break; case BinOp::Gt: res = a > c; break;
                case BinOp::LtEq: res = a <= c; break; case BinOp::GtEq: res = a >= c; break;
                default: break;
                }
            }
            else numFail();
            return Value(res);
        }
        switch (b->op) {
        case BinOp::Add:
            if (l.isInt() && r.isInt()) return Value(l.asInt() + r.asInt());
            if (l.isNumber() && r.isNumber()) return Value(l.asDouble() + r.asDouble());
            if (l.isString() && r.isString()) return Value(l.asString() + r.asString());
            if (l.isList() && r.isList()) {
                auto out = std::make_shared<ListValue>();
                out->items = l.asList()->items;
                for (auto& v : r.asList()->items) out->items.push_back(v);
                return Value(out);
            }
            numFail();
        case BinOp::Sub:
            if (l.isInt() && r.isInt()) return Value(l.asInt() - r.asInt());
            if (l.isNumber() && r.isNumber()) return Value(l.asDouble() - r.asDouble());
            numFail();
        case BinOp::Mul: {
            if (l.isInt() && r.isInt()) return Value(l.asInt() * r.asInt());
            if (l.isNumber() && r.isNumber()) return Value(l.asDouble() * r.asDouble());
            if (l.isString() && r.isInt()) {
                std::string o; for (long long i = 0; i < r.asInt(); ++i) o += l.asString();
                return Value(std::move(o));
            }
            if (l.isInt() && r.isString()) {
                std::string o; for (long long i = 0; i < l.asInt(); ++i) o += r.asString();
                return Value(std::move(o));
            }
            if (l.isList() && r.isInt()) {
                auto out = std::make_shared<ListValue>();
                for (long long i = 0; i < r.asInt(); ++i)
                    for (auto& v : l.asList()->items) out->items.push_back(v);
                return Value(out);
            }
            if (l.isInt() && r.isList()) {
                auto out = std::make_shared<ListValue>();
                for (long long i = 0; i < l.asInt(); ++i)
                    for (auto& v : r.asList()->items) out->items.push_back(v);
                return Value(out);
            }
            numFail();
        }
        case BinOp::Div: {
            if (l.isNumber() && r.isNumber()) {
                double d = r.asDouble();
                if (d == 0.0) throw RuntimeError("division by zero", b->loc);
                return Value(l.asDouble() / d);
            } numFail();
        }
        case BinOp::FloorDiv: {
            if (l.isNumber() && r.isNumber()) {
                double d = r.asDouble();
                if (d == 0.0) throw RuntimeError("division by zero", b->loc);
                double q = std::floor(l.asDouble() / d);
                if (l.isInt() && r.isInt()) return Value((long long)q);
                return Value(q);
            } numFail();
        }
        case BinOp::Mod: {
            if (l.isNumber() && r.isNumber()) {
                double d = r.asDouble();
                if (d == 0.0) throw RuntimeError("modulo by zero", b->loc);
                double m = std::fmod(l.asDouble(), d);
                if (m != 0 && ((m < 0) != (d < 0))) m += d;
                if (l.isInt() && r.isInt()) return Value((long long)m);
                return Value(m);
            } numFail();
        }
        case BinOp::Pow: {
            if (l.isNumber() && r.isNumber()) {
                if (l.isInt() && r.isInt() && r.asInt() >= 0) {
                    long long base = l.asInt(), exp = r.asInt(), acc = 1;
                    while (exp--) acc *= base;
                    return Value(acc);
                }
                return Value(std::pow(l.asDouble(), r.asDouble()));
            } numFail();
        }
        case BinOp::BAnd: case BinOp::BOr: case BinOp::BXor:
        case BinOp::Shl:  case BinOp::Shr: {
            if (!l.isInt() || !r.isInt()) numFail();
            long long a = l.asInt(), c = r.asInt();
            switch (b->op) {
            case BinOp::BAnd: return Value(a & c);
            case BinOp::BOr:  return Value(a | c);
            case BinOp::BXor: return Value(a ^ c);
            case BinOp::Shl:  return Value(a << c);
            case BinOp::Shr:  return Value(a >> c);
            default: break;
            }
            numFail();
        }
        case BinOp::Is:
            throw RuntimeError("'is' not yet supported", b->loc);
        default: break;
        }
        numFail(); return Value();
    }

    // ===========================================================================
    // Calls
    // ===========================================================================

    Value Interpreter::callValue(const Value& callee,
        const std::vector<Value>& args,
        SourceLocation loc) {
        if (!callee.isCallable())
            throw RuntimeError("attempt to call " + callee.typeName() + " value", loc);
        auto fn = callee.asCallable();
        switch (fn->kind) {
        case Callable::Kind::Native: return fn->nativeFn(args);
        case Callable::Kind::ClassCtor: {
            std::vector<std::pair<std::string, Value>> kwargs;
            for (auto& a : args) kwargs.emplace_back("", a);
            return constructInstance(fn->classObj, kwargs, loc);
        }
        case Callable::Kind::User:   return callUser(fn, args, loc);
        case Callable::Kind::VMFunction:
            if (vmRunner_) return vmRunner_(fn, args);
            throw RuntimeError("VM function invoked without VM context", loc);
        case Callable::Kind::Lambda: return callLambda(fn, args, loc);
        case Callable::Kind::BoundMethod: {
            std::vector<Value> all;
            all.reserve(args.size() + 1);
            all.push_back(Value(fn->boundSelf));
            for (auto& a : args) all.push_back(a);
            auto inner = std::make_shared<Callable>(*fn->methodFn);
            inner->definingClass = fn->definingClass;
            inner->closure = globals_;
            return callUser(inner, all, loc);
        }
        case Callable::Kind::SuperMethod:
            throw RuntimeError("cannot call super() directly", loc);
        case Callable::Kind::ListMethod:   return callListMethod(fn, args, loc);
        case Callable::Kind::MapMethod:    return callMapMethod(fn, args, loc);
        case Callable::Kind::StringMethod: return callStringMethod(fn, args, loc);
        }
        return Value();
    }

    Value Interpreter::callLambda(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args,
        SourceLocation loc) {
        const LambdaExpr* lam = fn->lambdaExpr;
        if (args.size() != lam->params.size())
            throw RuntimeError("lambda expects " + std::to_string(lam->params.size()) +
                " argument(s), got " + std::to_string(args.size()), loc);
        auto callEnv = std::make_shared<Environment>(fn->closure ? fn->closure : globals_);
        for (size_t i = 0; i < args.size(); ++i)
            callEnv->define(lam->params[i], args[i]);
        EnvGuard guard(env_, callEnv);
        return eval(lam->body.get());
    }

    // ===========================================================================
    // List / Map / String methods
    // ===========================================================================

    Value Interpreter::callListMethod(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args,
        SourceLocation loc) {
        auto lst = fn->boundList;
        const std::string& m = fn->name;
        if (m == "append") {
            if (args.size() != 1) throw RuntimeError("append() takes 1 argument", loc);
            lst->items.push_back(args[0]); return Value();
        }
        if (m == "pop") {
            if (args.empty()) {
                if (lst->items.empty()) throw RuntimeError("pop from empty list", loc);
                Value v = lst->items.back(); lst->items.pop_back(); return v;
            }
            if (args.size() != 1 || !args[0].isInt())
                throw RuntimeError("pop() takes 0 or 1 int argument", loc);
            long long i = args[0].asInt();
            if (i < 0) i += (long long)lst->items.size();
            if (i < 0 || i >= (long long)lst->items.size())
                throw RuntimeError("pop index out of range", loc);
            Value v = lst->items[(size_t)i];
            lst->items.erase(lst->items.begin() + i);
            return v;
        }
        if (m == "clear") {
            if (!args.empty()) throw RuntimeError("clear() takes no arguments", loc);
            lst->items.clear(); return Value();
        }
        if (m == "insert") {
            if (args.size() != 2 || !args[0].isInt())
                throw RuntimeError("insert(index, value) takes (int, T)", loc);
            long long i = args[0].asInt();
            if (i < 0) i += (long long)lst->items.size();
            if (i < 0) i = 0;
            if (i > (long long)lst->items.size()) i = (long long)lst->items.size();
            lst->items.insert(lst->items.begin() + i, args[1]);
            return Value();
        }
        if (m == "remove") {
            if (args.size() != 1) throw RuntimeError("remove() takes 1 argument", loc);
            for (size_t i = 0; i < lst->items.size(); ++i)
                if (valueEquals(lst->items[i], args[0])) {
                    lst->items.erase(lst->items.begin() + i); return Value();
                }
            throw RuntimeError("remove(): value not in list", loc);
        }
        if (m == "contains") {
            if (args.size() != 1) throw RuntimeError("contains() takes 1 argument", loc);
            for (auto& v : lst->items) if (valueEquals(v, args[0])) return Value(true);
            return Value(false);
        }
        if (m == "index") {
            if (args.size() != 1) throw RuntimeError("index() takes 1 argument", loc);
            for (size_t i = 0; i < lst->items.size(); ++i)
                if (valueEquals(lst->items[i], args[0])) return Value((long long)i);
            throw RuntimeError("index(): value not in list", loc);
        }
        throw RuntimeError("list has no method '" + m + "'", loc);
    }

    Value Interpreter::callMapMethod(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args,
        SourceLocation loc) {
        auto m = fn->boundMap;
        const std::string& name = fn->name;
        if (name == "put") {
            if (args.size() != 2 || !args[0].isString())
                throw RuntimeError("put(key, value) takes (str, T)", loc);
            m->entries[args[0].asString()] = args[1]; return Value();
        }
        if (name == "get") {
            if (args.size() != 1 || !args[0].isString())
                throw RuntimeError("get(key) takes one str argument", loc);
            auto it = m->entries.find(args[0].asString());
            if (it == m->entries.end())
                throw RuntimeError("map has no key '" + args[0].asString() + "'", loc);
            return it->second;
        }
        if (name == "remove") {
            if (args.size() != 1 || !args[0].isString())
                throw RuntimeError("remove(key) takes one str argument", loc);
            auto it = m->entries.find(args[0].asString());
            if (it == m->entries.end())
                throw RuntimeError("map has no key '" + args[0].asString() + "'", loc);
            m->entries.erase(it); return Value();
        }
        if (name == "contains") {
            if (args.size() != 1 || !args[0].isString())
                throw RuntimeError("contains(key) takes one str argument", loc);
            return Value(m->entries.count(args[0].asString()) > 0);
        }
        if (name == "keys") {
            if (!args.empty()) throw RuntimeError("keys() takes no arguments", loc);
            auto lst = std::make_shared<ListValue>();
            for (auto& [k, _] : m->entries) lst->items.push_back(Value(k));
            return Value(lst);
        }
        if (name == "values") {
            if (!args.empty()) throw RuntimeError("values() takes no arguments", loc);
            auto lst = std::make_shared<ListValue>();
            for (auto& [_, v] : m->entries) lst->items.push_back(v);
            return Value(lst);
        }
        if (name == "clear") {
            if (!args.empty()) throw RuntimeError("clear() takes no arguments", loc);
            m->entries.clear(); return Value();
        }
        throw RuntimeError("map has no method '" + name + "'", loc);
    }

    namespace {
        std::string toLower(const std::string& s) {
            std::string r = s;
            for (auto& c : r) c = (char)std::tolower((unsigned char)c);
            return r;
        }
        std::string toUpper(const std::string& s) {
            std::string r = s;
            for (auto& c : r) c = (char)std::toupper((unsigned char)c);
            return r;
        }
        bool isWS(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
    } // namespace

    Value Interpreter::callStringMethod(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args,
        SourceLocation loc) {
        const std::string& s = fn->boundStr;
        const std::string& m = fn->name;
        auto noArgs = [&]() { if (!args.empty()) throw RuntimeError(m + "() takes no arguments", loc); };
        auto oneStr = [&](const char* what) -> const std::string& {
            if (args.size() != 1 || !args[0].isString())
                throw RuntimeError(std::string(m) + "() takes one str argument (" + what + ")", loc);
            return args[0].asString();
            };
        if (m == "upper") { noArgs(); return Value(toUpper(s)); }
        if (m == "lower") { noArgs(); return Value(toLower(s)); }
        if (m == "strip") {
            if (args.empty()) {
                size_t a = 0, b = s.size();
                while (a < b && isWS(s[a])) ++a;
                while (b > a && isWS(s[b - 1])) --b;
                return Value(s.substr(a, b - a));
            }
            if (args.size() == 1 && args[0].isString()) {
                const std::string& chars = args[0].asString();
                size_t a = 0, b = s.size();
                while (a < b && chars.find(s[a]) != std::string::npos) ++a;
                while (b > a && chars.find(s[b - 1]) != std::string::npos) --b;
                return Value(s.substr(a, b - a));
            }
            throw RuntimeError("strip() takes 0 or 1 str argument", loc);
        }
        if (m == "lstrip") { noArgs(); size_t a = 0; while (a < s.size() && isWS(s[a])) ++a; return Value(s.substr(a)); }
        if (m == "rstrip") { noArgs(); size_t b = s.size(); while (b > 0 && isWS(s[b - 1])) --b; return Value(s.substr(0, b)); }
        if (m == "split") {
            auto lst = std::make_shared<ListValue>();
            if (args.empty()) {
                size_t i = 0;
                while (i < s.size()) {
                    while (i < s.size() && isWS(s[i])) ++i;
                    if (i >= s.size()) break;
                    size_t start = i;
                    while (i < s.size() && !isWS(s[i])) ++i;
                    lst->items.push_back(Value(s.substr(start, i - start)));
                }
                return Value(lst);
            }
            const std::string& sep = oneStr("separator");
            if (sep.empty()) {
                for (char c : s) lst->items.push_back(Value(std::string(1, c)));
                return Value(lst);
            }
            size_t pos = 0, next;
            while ((next = s.find(sep, pos)) != std::string::npos) {
                lst->items.push_back(Value(s.substr(pos, next - pos)));
                pos = next + sep.size();
            }
            lst->items.push_back(Value(s.substr(pos)));
            return Value(lst);
        }
        if (m == "join") {
            if (args.size() != 1 || !args[0].isList())
                throw RuntimeError("join() takes a list argument", loc);
            auto lst = args[0].asList();
            std::string out;
            for (size_t i = 0; i < lst->items.size(); ++i) {
                if (i) out += s;
                if (!lst->items[i].isString()) throw RuntimeError("join(): all elements must be str", loc);
                out += lst->items[i].asString();
            }
            return Value(std::move(out));
        }
        if (m == "replace") {
            if (args.size() != 2 || !args[0].isString() || !args[1].isString())
                throw RuntimeError("replace(old, new) takes two str arguments", loc);
            const std::string& oldS = args[0].asString();
            const std::string& newS = args[1].asString();
            if (oldS.empty()) return Value(s);
            std::string out;
            size_t pos = 0, next;
            while ((next = s.find(oldS, pos)) != std::string::npos) {
                out += s.substr(pos, next - pos); out += newS; pos = next + oldS.size();
            }
            out += s.substr(pos);
            return Value(std::move(out));
        }
        if (m == "find") { const std::string& sub = oneStr("substring"); auto p = s.find(sub); return Value(p == std::string::npos ? (long long)-1 : (long long)p); }
        if (m == "contains") { const std::string& sub = oneStr("substring"); return Value(s.find(sub) != std::string::npos); }
        if (m == "starts_with") { const std::string& p = oneStr("prefix"); return Value(s.size() >= p.size() && s.compare(0, p.size(), p) == 0); }
        if (m == "ends_with") { const std::string& p = oneStr("suffix"); return Value(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0); }
        if (m == "is_digit") { noArgs(); if (s.empty()) return Value(false); for (char c : s) if (!std::isdigit((unsigned char)c)) return Value(false); return Value(true); }
        if (m == "is_alpha") { noArgs(); if (s.empty()) return Value(false); for (char c : s) if (!std::isalpha((unsigned char)c)) return Value(false); return Value(true); }
        if (m == "is_space") { noArgs(); if (s.empty()) return Value(false); for (char c : s) if (!isWS(c)) return Value(false); return Value(true); }
        if (m == "char_at") {
            if (args.size() != 1 || !args[0].isInt()) throw RuntimeError("char_at(i) takes one int argument", loc);
            long long i = args[0].asInt();
            if (i < 0) i += (long long)s.size();
            if (i < 0 || i >= (long long)s.size()) throw RuntimeError("char_at: index out of range", loc);
            return Value(std::string(1, s[(size_t)i]));
        }
        throw RuntimeError("str has no method '" + m + "'", loc);
    }

    // ===========================================================================
    // User / method calls
    // ===========================================================================

    Value Interpreter::callUser(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args,
        SourceLocation loc) {
        const DefStmt* d = fn->decl;
        if (d->isGenerator) return createGenerator(fn, args);
        if (args.size() != d->params.size())
            throw RuntimeError("function '" + fn->name + "' expects " +
                std::to_string(d->params.size()) + " argument(s), got " +
                std::to_string(args.size()), loc);
        auto callEnv = std::make_shared<Environment>(fn->closure ? fn->closure : globals_);
        for (size_t i = 0; i < args.size(); ++i)
            callEnv->define(d->params[i].name, args[i]);
        if (fn->definingClass) callEnv->define("__class__", Value(fn->definingClass));
        EnvGuard guard(env_, callEnv);
        try { execBlock(d->body); }
        catch (ReturnSignal& r) { return r.value; }
        return Value();
    }

    // ===========================================================================
    // Builtins
    // ===========================================================================

    namespace {

        Value bi_print(const std::vector<Value>& args) {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) std::cout << ' ';
                std::cout << args[i].toString();
            }
            std::cout << '\n'; return Value();
        }
        Value bi_str(const std::vector<Value>& a) { return a.empty() ? Value("") : Value(a[0].toString()); }
        Value bi_bool(const std::vector<Value>& a) { return a.empty() ? Value(false) : Value(a[0].truthy()); }
        Value bi_int(const std::vector<Value>& a) {
            if (a.empty()) return Value(0LL);
            const Value& v = a[0];
            if (v.isInt()) return v;
            if (v.isFloat()) return Value((long long)v.asFloat());
            if (v.isBool()) return Value((long long)(v.asBool() ? 1 : 0));
            if (v.isString()) { try { return Value((long long)std::stoll(v.asString())); } catch (...) {} }
            throw std::runtime_error("int(): cannot convert " + v.typeName());
        }
        Value bi_float(const std::vector<Value>& a) {
            if (a.empty()) return Value(0.0);
            const Value& v = a[0];
            if (v.isFloat()) return v;
            if (v.isInt()) return Value((double)v.asInt());
            if (v.isBool()) return Value(v.asBool() ? 1.0 : 0.0);
            if (v.isString()) { try { return Value(std::stod(v.asString())); } catch (...) {} }
            throw std::runtime_error("float(): cannot convert " + v.typeName());
        }
        Value bi_len(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("len() takes exactly 1 argument");
            if (a[0].isString()) return Value((long long)a[0].asString().size());
            if (a[0].isList())   return Value((long long)a[0].asList()->items.size());
            if (a[0].isMap())    return Value((long long)a[0].asMap()->entries.size());
            throw std::runtime_error("len(): unsupported type " + a[0].typeName());
        }
        Value bi_abs(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isNumber()) throw std::runtime_error("abs() expects number");
            if (a[0].isInt()) return Value(std::llabs(a[0].asInt()));
            return Value(std::fabs(a[0].asFloat()));
        }
        Value bi_type(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("type() takes 1 argument");
            return Value(a[0].typeName());
        }
        Value bi_min(const std::vector<Value>& a) {
            if (a.empty()) throw std::runtime_error("min() requires at least one argument");
            Value b = a[0];
            for (size_t i = 1; i < a.size(); ++i) {
                if (a[i].isNumber() && b.isNumber()) { if (a[i].asDouble() < b.asDouble()) b = a[i]; }
                else if (a[i].isString() && b.isString()) { if (a[i].asString() < b.asString()) b = a[i]; }
                else throw std::runtime_error("min(): mixed types");
            } return b;
        }
        Value bi_max(const std::vector<Value>& a) {
            if (a.empty()) throw std::runtime_error("max() requires at least one argument");
            Value b = a[0];
            for (size_t i = 1; i < a.size(); ++i) {
                if (a[i].isNumber() && b.isNumber()) { if (a[i].asDouble() > b.asDouble()) b = a[i]; }
                else if (a[i].isString() && b.isString()) { if (a[i].asString() > b.asString()) b = a[i]; }
                else throw std::runtime_error("max(): mixed types");
            } return b;
        }
        Value bi_input(const std::vector<Value>& a) {
            if (a.size() > 1)
                throw std::runtime_error("input() takes 0 or 1 argument");
            if (!a.empty()) {
                if (!a[0].isString())
                    throw std::runtime_error("input() prompt must be a string");
                std::cout << a[0].asString();
                std::cout.flush();
            }
            std::string line;
            if (!std::getline(std::cin, line)) return Value("");
            return Value(std::move(line));
        }

        Value bi_read_line(const std::vector<Value>& a) {
            if (!a.empty())
                throw std::runtime_error("read_line() takes no arguments");
            std::string line;
            if (!std::getline(std::cin, line)) return Value("");
            return Value(std::move(line));
        }

        Value bi_read_int(const std::vector<Value>& a) {
            if (!a.empty())
                throw std::runtime_error("read_int() takes no arguments");
            std::string line;
            if (!std::getline(std::cin, line)) return Value(0LL);
            try { return Value((long long)std::stoll(line)); }
            catch (...) {
                throw std::runtime_error("read_int: invalid integer '" + line + "'");
            }
        }

        Value bi_read_all(const std::vector<Value>& a) {
            if (!a.empty())
                throw std::runtime_error("read_all() takes no arguments");
            std::stringstream ss;
            ss << std::cin.rdbuf();
            return Value(ss.str());
        }
        Value bi_range(const std::vector<Value>& a) {
            if (a.empty() || a.size() > 3)
                throw std::runtime_error("range() takes 1 to 3 arguments");
            long long start = 0, stop = 0, step = 1;
            if (a.size() == 1) {
                if (!a[0].isInt()) throw std::runtime_error("range() requires int arguments");
                stop = a[0].asInt();
            }
            else if (a.size() == 2) {
                if (!a[0].isInt() || !a[1].isInt()) throw std::runtime_error("range() requires int arguments");
                start = a[0].asInt(); stop = a[1].asInt();
            }
            else {
                if (!a[0].isInt() || !a[1].isInt() || !a[2].isInt()) throw std::runtime_error("range() requires int arguments");
                start = a[0].asInt(); stop = a[1].asInt(); step = a[2].asInt();
                if (step == 0) throw std::runtime_error("range() step cannot be 0");
            }
            auto lst = std::make_shared<ListValue>();
            if (step > 0) for (long long i = start; i < stop; i += step) lst->items.emplace_back(i);
            else          for (long long i = start; i > stop; i += step) lst->items.emplace_back(i);
            return Value(lst);
        }
        Value bi_ord(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isString() || a[0].asString().size() != 1)
                throw std::runtime_error("ord() takes one single-character string");
            return Value((long long)(unsigned char)a[0].asString()[0]);
        }
        Value bi_chr(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isInt()) throw std::runtime_error("chr() takes one int argument");
            long long code = a[0].asInt();
            if (code < 0 || code > 255) throw std::runtime_error("chr() argument out of range (0-255)");
            return Value(std::string(1, (char)code));
        }
        Value bi_list(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("list() takes 1 argument");
            const Value& v = a[0];
            if (v.isList()) return v;
            if (v.isString()) {
                auto lst = std::make_shared<ListValue>();
                for (char c : v.asString()) lst->items.push_back(Value(std::string(1, c)));
                return Value(lst);
            }
            throw std::runtime_error("list(): cannot convert " + v.typeName());
        }

        // ---- higher-order ----
        Value bi_map(const std::vector<Value>& args) {
            if (args.size() != 2) throw std::runtime_error("map() takes exactly 2 arguments");
            if (!args[0].isCallable()) throw std::runtime_error("map() first argument must be callable");
            if (!args[1].isList())     throw std::runtime_error("map() second argument must be a list");
            auto out = std::make_shared<ListValue>();
            out->items.reserve(args[1].asList()->items.size());
            for (auto& v : args[1].asList()->items)
                out->items.push_back(Interpreter::current_->callValue(args[0], { v }, SourceLocation{}));
            return Value(out);
        }
        Value bi_filter(const std::vector<Value>& args) {
            if (args.size() != 2) throw std::runtime_error("filter() takes exactly 2 arguments");
            if (!args[0].isCallable()) throw std::runtime_error("filter() first argument must be callable");
            if (!args[1].isList())     throw std::runtime_error("filter() second argument must be a list");
            auto out = std::make_shared<ListValue>();
            for (auto& v : args[1].asList()->items) {
                Value keep = Interpreter::current_->callValue(args[0], { v }, SourceLocation{});
                if (keep.truthy()) out->items.push_back(v);
            }
            return Value(out);
        }
        Value bi_sorted(const std::vector<Value>& args) {
            if (args.empty() || args.size() > 2)
                throw std::runtime_error("sorted() takes 1 or 2 arguments");
            if (!args[0].isList())
                throw std::runtime_error("sorted() first argument must be a list");
            auto out = std::make_shared<ListValue>();
            out->items = args[0].asList()->items;
            std::function<Value(const Value&)> keyFn;
            if (args.size() == 2) {
                if (!args[1].isCallable())
                    throw std::runtime_error("sorted() second argument must be callable");
                Value f = args[1];
                keyFn = [f](const Value& v) {
                    return Interpreter::current_->callValue(f, { v }, SourceLocation{});
                    };
            }
            auto cmp = [&](const Value& a, const Value& b) -> bool {
                Value ka = keyFn ? keyFn(a) : a;
                Value kb = keyFn ? keyFn(b) : b;
                if (ka.isNumber() && kb.isNumber()) return ka.asDouble() < kb.asDouble();
                if (ka.isString() && kb.isString()) return ka.asString() < kb.asString();
                throw std::runtime_error("sorted(): elements are not comparable");
                };
            std::stable_sort(out->items.begin(), out->items.end(), cmp);
            return Value(out);
        }
        Value bi_reduce(const std::vector<Value>& args) {
            if (args.size() != 2) throw std::runtime_error("reduce() takes exactly 2 arguments");
            if (!args[0].isCallable()) throw std::runtime_error("reduce() first argument must be callable");
            if (!args[1].isList())     throw std::runtime_error("reduce() second argument must be a list");
            const auto& items = args[1].asList()->items;
            if (items.empty()) throw std::runtime_error("reduce() of empty list");
            Value acc = items[0];
            for (size_t i = 1; i < items.size(); ++i)
                acc = Interpreter::current_->callValue(args[0], { acc, items[i] }, SourceLocation{});
            return acc;
        }
        Value bi_any(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isList())
                throw std::runtime_error("any() takes one list argument");
            for (auto& v : a[0].asList()->items) if (v.truthy()) return Value(true);
            return Value(false);
        }
        Value bi_all(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isList())
                throw std::runtime_error("all() takes one list argument");
            for (auto& v : a[0].asList()->items) if (!v.truthy()) return Value(false);
            return Value(true);
        }
        Value bi_sum(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isList())
                throw std::runtime_error("sum() takes one list argument");
            long long iAcc = 0;
            double    dAcc = 0.0;
            bool      anyFloat = false;
            for (auto& v : a[0].asList()->items) {
                if (!v.isNumber()) throw std::runtime_error("sum(): list contains non-number");
                if (v.isFloat()) { anyFloat = true; dAcc += v.asFloat(); }
                else iAcc += v.asInt();
            }
            if (anyFloat) return Value(dAcc + (double)iAcc);
            return Value(iAcc);
        }

        // ---- math ----
        Value m_sqrt(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isNumber()) throw std::runtime_error("math.sqrt expects a number");
            double x = a[0].asDouble();
            if (x < 0) throw std::runtime_error("math.sqrt of negative number");
            return Value(std::sqrt(x));
        }
        Value m_sin(const std::vector<Value>& a) { return Value(std::sin(a.at(0).asDouble())); }
        Value m_cos(const std::vector<Value>& a) { return Value(std::cos(a.at(0).asDouble())); }
        Value m_tan(const std::vector<Value>& a) { return Value(std::tan(a.at(0).asDouble())); }
        Value m_log(const std::vector<Value>& a) {
            if (a.empty()) throw std::runtime_error("math.log expects a number");
            double x = a[0].asDouble();
            if (x <= 0) throw std::runtime_error("math.log domain error");
            return Value(std::log(x));
        }
        Value m_log2(const std::vector<Value>& a) {
            if (a.empty()) throw std::runtime_error("math.log2 expects a number");
            double x = a[0].asDouble();
            if (x <= 0) throw std::runtime_error("math.log2 domain error");
            return Value(std::log2(x));
        }
        Value m_log10(const std::vector<Value>& a) {
            if (a.empty()) throw std::runtime_error("math.log10 expects a number");
            double x = a[0].asDouble();
            if (x <= 0) throw std::runtime_error("math.log10 domain error");
            return Value(std::log10(x));
        }
        Value m_exp(const std::vector<Value>& a) { return Value(std::exp(a.at(0).asDouble())); }
        Value m_floor(const std::vector<Value>& a) {
            if (a.empty()) throw std::runtime_error("math.floor expects a number");
            if (a[0].isInt()) return a[0];
            return Value((long long)std::floor(a[0].asDouble()));
        }
        Value m_ceil(const std::vector<Value>& a) {
            if (a.empty()) throw std::runtime_error("math.ceil expects a number");
            if (a[0].isInt()) return a[0];
            return Value((long long)std::ceil(a[0].asDouble()));
        }
        Value m_pow(const std::vector<Value>& a) {
            if (a.size() != 2) throw std::runtime_error("math.pow takes 2 arguments");
            return Value(std::pow(a[0].asDouble(), a[1].asDouble()));
        }
        Value m_asin(const std::vector<Value>& a) { return Value(std::asin(a.at(0).asDouble())); }
        Value m_acos(const std::vector<Value>& a) { return Value(std::acos(a.at(0).asDouble())); }
        Value m_atan(const std::vector<Value>& a) { return Value(std::atan(a.at(0).asDouble())); }
        Value m_atan2(const std::vector<Value>& a) {
            if (a.size() != 2) throw std::runtime_error("math.atan2 takes 2 arguments");
            return Value(std::atan2(a[0].asDouble(), a[1].asDouble()));
        }
        Value m_sinh(const std::vector<Value>& a) { return Value(std::sinh(a.at(0).asDouble())); }
        Value m_cosh(const std::vector<Value>& a) { return Value(std::cosh(a.at(0).asDouble())); }
        Value m_tanh(const std::vector<Value>& a) { return Value(std::tanh(a.at(0).asDouble())); }
        Value m_asinh(const std::vector<Value>& a) { return Value(std::asinh(a.at(0).asDouble())); }
        Value m_acosh(const std::vector<Value>& a) { return Value(std::acosh(a.at(0).asDouble())); }
        Value m_atanh(const std::vector<Value>& a) { return Value(std::atanh(a.at(0).asDouble())); }
        Value m_degrees(const std::vector<Value>& a) {
            return Value(a.at(0).asDouble() * 180.0 / 3.14159265358979323846);
        }
        Value m_radians(const std::vector<Value>& a) {
            return Value(a.at(0).asDouble() * 3.14159265358979323846 / 180.0);
        }
        Value m_hypot(const std::vector<Value>& a) {
            if (a.size() != 2) throw std::runtime_error("math.hypot takes 2 arguments");
            return Value(std::hypot(a[0].asDouble(), a[1].asDouble()));
        }
        Value m_fmod(const std::vector<Value>& a) {
            if (a.size() != 2) throw std::runtime_error("math.fmod takes 2 arguments");
            return Value(std::fmod(a[0].asDouble(), a[1].asDouble()));
        }
        Value m_trunc(const std::vector<Value>& a) {
            if (a.at(0).isInt()) return a[0];
            return Value((long long)std::trunc(a[0].asDouble()));
        }
        Value m_isnan(const std::vector<Value>& a) {
            return Value(a.at(0).isFloat() && std::isnan(a[0].asFloat()));
        }
        Value m_isinf(const std::vector<Value>& a) {
            return Value(a.at(0).isFloat() && std::isinf(a[0].asFloat()));
        }
        Value m_isfinite(const std::vector<Value>& a) {
            if (a.at(0).isInt()) return Value(true);
            return Value(a.at(0).isFloat() && std::isfinite(a[0].asFloat()));
        }
        // ---- Phase 13.0 — inspection & pure helpers ----

        uint64_t fnv1a64(const std::string& s) {
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
            return h;
        }
        long long mixInt64(long long x) {
            uint64_t h = (uint64_t)x;
            h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return (long long)h;
        }

        Value bi_hash(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("hash() takes 1 argument");
            const Value& v = a[0];
            if (v.isInt())    return Value(mixInt64(v.asInt()));
            if (v.isFloat())  return Value(mixInt64((long long)v.asFloat()));
            if (v.isBool())   return Value((long long)(v.asBool() ? 1 : 0));
            if (v.isNone())   return Value((long long)0);
            if (v.isString()) return Value((long long)fnv1a64(v.asString()));
            if (v.isList())   return Value((long long)(uintptr_t)v.asList().get());
            if (v.isMap())    return Value((long long)(uintptr_t)v.asMap().get());
            if (v.isInstance()) return Value((long long)(uintptr_t)v.asInstance().get());
            if (v.isCallable()) return Value((long long)(uintptr_t)v.asCallable().get());
            if (v.isClass())  return Value((long long)(uintptr_t)v.asClass().get());
            if (v.isModule()) return Value((long long)(uintptr_t)v.asModule().get());
            if (v.isGenerator()) return Value((long long)(uintptr_t)v.asGenerator().get());
            return Value((long long)0);
        }
        Value bi_id(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("id() takes 1 argument");
            const Value& v = a[0];
            if (v.isInt())    return Value((long long)v.asInt());
            if (v.isFloat())  return Value((long long)v.asFloat());
            if (v.isBool())   return Value((long long)(v.asBool() ? 1 : 0));
            if (v.isNone())   return Value((long long)0);
            if (v.isString()) return Value((long long)(uintptr_t)&v);
            if (v.isList())   return Value((long long)(uintptr_t)v.asList().get());
            if (v.isMap())    return Value((long long)(uintptr_t)v.asMap().get());
            if (v.isInstance()) return Value((long long)(uintptr_t)v.asInstance().get());
            if (v.isCallable()) return Value((long long)(uintptr_t)v.asCallable().get());
            if (v.isClass())  return Value((long long)(uintptr_t)v.asClass().get());
            if (v.isModule()) return Value((long long)(uintptr_t)v.asModule().get());
            if (v.isGenerator()) return Value((long long)(uintptr_t)v.asGenerator().get());
            return Value((long long)0);
        }
        Value bi_callable(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("callable() takes 1 argument");
            return Value(a[0].isCallable());
        }

        bool nameMatchesClass(Interpreter* I, const Value& v, const std::string& n) {
            if (n == "int")   return v.isInt();
            if (n == "float") return v.isFloat();
            if (n == "bool")  return v.isBool();
            if (n == "str")   return v.isString();
            if (n == "None")  return v.isNone();
            if (n == "list")  return v.isList();
            if (n == "map")   return v.isMap();
            if (v.isInstance() && v.asInstance()->cls) {
                for (auto c = v.asInstance()->cls; c; c = c->parent)
                    if (c->name == n) return true;
            }
            (void)I;
            return false;
        }
        Value bi_isinstance(const std::vector<Value>& a) {
            if (a.size() != 2) throw std::runtime_error("isinstance() takes 2 arguments");
            const Value& v = a[0];
            const Value& t = a[1];
            if (t.isString())
                return Value(nameMatchesClass(Interpreter::current_, v, t.asString()));
            if (t.isClass()) {
                if (!v.isInstance()) return Value(false);
                for (auto c = v.asInstance()->cls; c; c = c->parent)
                    if (c == t.asClass() || c->name == t.asClass()->name)
                        return Value(true);
                return Value(false);
            }
            if (t.isCallable() && t.asCallable()->kind == Callable::Kind::ClassCtor) {
                auto cls = t.asCallable()->classObj;
                if (!v.isInstance()) return Value(false);
                for (auto c = v.asInstance()->cls; c; c = c->parent)
                    if (c == cls || c->name == cls->name) return Value(true);
                return Value(false);
            }
            return Value(false);
        }
        Value bi_issubclass(const std::vector<Value>& a) {
            if (a.size() != 2) throw std::runtime_error("issubclass() takes 2 arguments");
            auto asCls = [](const Value& v) -> std::shared_ptr<ClassObject> {
                if (v.isClass()) return v.asClass();
                if (v.isCallable() &&
                    v.asCallable()->kind == Callable::Kind::ClassCtor)
                    return v.asCallable()->classObj;
                return nullptr;
                };
            auto A = asCls(a[0]);
            auto B = asCls(a[1]);
            if (!A || !B) return Value(false);
            for (auto c = A; c; c = c->parent)
                if (c == B || c->name == B->name) return Value(true);
            return Value(false);
        }
        Value bi_getattr(const std::vector<Value>& a) {
            if (a.size() != 2 || !a[1].isString())
                throw std::runtime_error("getattr(obj, name) takes (any, str)");
            return Interpreter::current_->vmGetAttr(a[0], a[1].asString(),
                SourceLocation{});
        }
        Value bi_hasattr(const std::vector<Value>& a) {
            if (a.size() != 2 || !a[1].isString())
                throw std::runtime_error("hasattr(obj, name) takes (any, str)");
            try {
                (void)Interpreter::current_->vmGetAttr(a[0], a[1].asString(),
                    SourceLocation{});
                return Value(true);
            }
            catch (...) { return Value(false); }
        }
        Value bi_dir(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("dir() takes 1 argument");
            std::vector<std::string> names;
            const Value& v = a[0];
            if (v.isInstance()) {
                auto si = v.asInstance();
                for (auto& kv : si->fields) names.push_back(kv.first);
                for (auto c = si->cls; c; c = c->parent) {
                    auto it = Interpreter::current_->classDecls_.find(c->name);
                    if (it == Interpreter::current_->classDecls_.end()) continue;
                    for (auto& m : it->second->methods)
                        names.push_back(m->name);
                }
            }
            else if (v.isModule()) {
                for (auto& kv : v.asModule()->members) names.push_back(kv.first);
            }
            else if (v.isString()) {
                names = { "upper", "lower", "strip", "split", "join",
                          "replace", "find", "contains", "starts_with",
                          "ends_with", "is_digit", "is_alpha", "is_space",
                          "char_at", "to_int", "substr", "lstrip", "rstrip" };
            }
            else if (v.isList()) {
                names = { "append", "pop", "clear", "insert", "remove",
                          "contains", "index" };
            }
            else if (v.isMap()) {
                names = { "put", "get", "remove", "contains", "keys",
                          "values", "clear" };
            }
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            auto out = std::make_shared<ListValue>();
            for (auto& n : names) out->items.push_back(Value(n));
            return Value(out);
        }
        Value bi_repr(const std::vector<Value>& a) {
            if (a.size() != 1) throw std::runtime_error("repr() takes 1 argument");
            return Value(a[0].toString());
        }
        Value bi_enumerate(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isList())
                throw std::runtime_error("enumerate() takes a list");
            auto out = std::make_shared<ListValue>();
            auto src = a[0].asList();
            for (size_t i = 0; i < src->items.size(); ++i) {
                auto pair = std::make_shared<ListValue>();
                pair->items.push_back(Value((long long)i));
                pair->items.push_back(src->items[i]);
                out->items.push_back(Value(pair));
            }
            return Value(out);
        }
        Value bi_zip(const std::vector<Value>& a) {
            if (a.size() != 2 || !a[0].isList() || !a[1].isList())
                throw std::runtime_error("zip(a, b) takes two lists");
            auto out = std::make_shared<ListValue>();
            auto x = a[0].asList();
            auto y = a[1].asList();
            size_t n = std::min(x->items.size(), y->items.size());
            for (size_t i = 0; i < n; ++i) {
                auto pair = std::make_shared<ListValue>();
                pair->items.push_back(x->items[i]);
                pair->items.push_back(y->items[i]);
                out->items.push_back(Value(pair));
            }
            return Value(out);
        }
        Value bi_reversed(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isList())
                throw std::runtime_error("reversed() takes a list");
            auto out = std::make_shared<ListValue>();
            auto src = a[0].asList();
            out->items.assign(src->items.rbegin(), src->items.rend());
            return Value(out);
        }
        Value bi_round(const std::vector<Value>& a) {
            if (a.empty() || a.size() > 2)
                throw std::runtime_error("round() takes 1 or 2 arguments");
            double x = a[0].isInt() ? (double)a[0].asInt() : a[0].asFloat();
            if (a.size() == 1) {
                return Value((long long)std::llround(x));
            }
            if (!a[1].isInt()) throw std::runtime_error("round() digits must be int");
            long long d = a[1].asInt();
            double scale = std::pow(10.0, (double)d);
            double r = std::round(x * scale) / scale;
            if (d <= 0) return Value((long long)r);
            return Value(r);
        }
        Value bi_pow(const std::vector<Value>& a) {
            if (a.size() < 2 || a.size() > 3)
                throw std::runtime_error("pow() takes 2 or 3 arguments");
            if (a.size() == 3) {
                if (!a[0].isInt() || !a[1].isInt() || !a[2].isInt())
                    throw std::runtime_error("3-arg pow() requires ints");
                long long b = a[0].asInt(), e = a[1].asInt(), m = a[2].asInt();
                long long r = 1 % m;
                b %= m;
                while (e > 0) {
                    if (e & 1) r = (r * b) % m;
                    b = (b * b) % m;
                    e >>= 1;
                }
                return Value(r);
            }
            if (a[0].isInt() && a[1].isInt() && a[1].asInt() >= 0) {
                long long b = a[0].asInt(), e = a[1].asInt(), r = 1;
                while (e > 0) {
                    if (e & 1) r *= b;
                    b *= b;
                    e >>= 1;
                }
                return Value(r);
            }
            double b = a[0].isInt() ? (double)a[0].asInt() : a[0].asFloat();
            double e = a[1].isInt() ? (double)a[1].asInt() : a[1].asFloat();
            return Value(std::pow(b, e));
        }
        Value bi_divmod(const std::vector<Value>& a) {
            if (a.size() != 2 || !a[0].isNumber() || !a[1].isNumber())
                throw std::runtime_error("divmod(a, b) requires numbers");
            auto out = std::make_shared<ListValue>();
            if (a[0].isInt() && a[1].isInt()) {
                long long x = a[0].asInt(), y = a[1].asInt();
                if (y == 0) throw std::runtime_error("divmod: division by zero");
                long long q = x / y;
                long long r = x % y;
                if (r != 0 && ((r < 0) != (y < 0))) { q--; r += y; }
                out->items.push_back(Value(q));
                out->items.push_back(Value(r));
            }
            else {
                double x = a[0].asDouble(), y = a[1].asDouble();
                if (y == 0.0) throw std::runtime_error("divmod: division by zero");
                double q = std::floor(x / y);
                double r = x - q * y;
                out->items.push_back(Value(q));
                out->items.push_back(Value(r));
            }
            return Value(out);
        }
        Value bi_sign(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isNumber())
                throw std::runtime_error("sign() requires a number");
            double x = a[0].asDouble();
            return Value((long long)((x > 0) - (x < 0)));
        }
        long long igcd(long long a, long long b) {
            if (a < 0) a = -a;
            if (b < 0) b = -b;
            while (b) { long long t = a % b; a = b; b = t; }
            return a;
        }
        Value bi_gcd(const std::vector<Value>& a) {
            if (a.size() != 2 || !a[0].isInt() || !a[1].isInt())
                throw std::runtime_error("gcd(a, b) requires two ints");
            return Value(igcd(a[0].asInt(), a[1].asInt()));
        }
        Value bi_lcm(const std::vector<Value>& a) {
            if (a.size() != 2 || !a[0].isInt() || !a[1].isInt())
                throw std::runtime_error("lcm(a, b) requires two ints");
            long long x = a[0].asInt(), y = a[1].asInt();
            if (x == 0 || y == 0) return Value((long long)0);
            long long g = igcd(x, y);
            return Value((x / g) * y);
        }
        Value bi_clamp(const std::vector<Value>& a) {
            if (a.size() != 3 || !a[0].isNumber() || !a[1].isNumber() || !a[2].isNumber())
                throw std::runtime_error("clamp(x, lo, hi) requires three numbers");
            if (a[0].isInt() && a[1].isInt() && a[2].isInt()) {
                long long x = a[0].asInt(), lo = a[1].asInt(), hi = a[2].asInt();
                if (x < lo) return Value(lo);
                if (x > hi) return Value(hi);
                return Value(x);
            }
            double x = a[0].asDouble(), lo = a[1].asDouble(), hi = a[2].asDouble();
            if (x < lo) return Value(lo);
            if (x > hi) return Value(hi);
            return Value(x);
        }
    } // namespace

    namespace {
        Value bi_next(const std::vector<Value>& a) {
            if (a.size() != 1 || !a[0].isGenerator())
                throw std::runtime_error("next() takes a generator argument");
            return Interpreter::current_->nextGenerator(
                a[0].asGenerator(), SourceLocation{});
        }
    }

    void Interpreter::installBuiltins() {
        auto add = [&](const char* name, NativeFnPtr fn) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::Native; c->name = name; c->nativeFn = fn;
            globals_->define(name, Value(c));
            };
        add("print", bi_print);
        add("str", bi_str);
        add("bool", bi_bool);
        add("int", bi_int);
        add("float", bi_float);
        add("len", bi_len);
        add("abs", bi_abs);
        add("type", bi_type);
        add("min", bi_min);
        add("max", bi_max);
        add("input", bi_input);
        add("read_line", bi_read_line);
        add("read_int", bi_read_int);
        add("read_all", bi_read_all);
        add("range", bi_range);
        add("ord", bi_ord);
        add("chr", bi_chr);
        add("list", bi_list);
        add("map", bi_map);
        add("filter", bi_filter);
        add("sorted", bi_sorted);
        add("reduce", bi_reduce);
        add("any", bi_any);
        add("all", bi_all);
        add("sum", bi_sum);
        // Phase 13.0 — inspection & pure helpers.
        add("hash", bi_hash);
        add("id", bi_id);
        add("callable", bi_callable);
        add("isinstance", bi_isinstance);
        add("issubclass", bi_issubclass);
        add("getattr", bi_getattr);
        add("hasattr", bi_hasattr);
        add("dir", bi_dir);
        add("repr", bi_repr);
        add("enumerate", bi_enumerate);
        add("zip", bi_zip);
        add("reversed", bi_reversed);
        add("round", bi_round);
        add("pow", bi_pow);
        add("divmod", bi_divmod);
        add("sign", bi_sign);
        add("gcd", bi_gcd);
        add("lcm", bi_lcm);
        add("clamp", bi_clamp);
        add("next", bi_next);
    }

    void Interpreter::installMathModule() {
        auto mod = std::make_shared<ModuleValue>();
        mod->name = "math";
        auto addFn = [&](const char* name, NativeFnPtr fn) {
            auto c = std::make_shared<Callable>();
            c->kind = Callable::Kind::Native; c->name = name; c->nativeFn = fn;
            mod->members[name] = Value(c);
            };
        addFn("sqrt", m_sqrt);   addFn("sin", m_sin);   addFn("cos", m_cos);
        addFn("tan", m_tan);    addFn("log", m_log);   addFn("log2", m_log2);
        addFn("log10", m_log10);  addFn("exp", m_exp);   addFn("floor", m_floor);
        addFn("ceil", m_ceil);   addFn("pow", m_pow);
        addFn("asin", m_asin);   addFn("acos", m_acos);  addFn("atan", m_atan);
        addFn("atan2", m_atan2); addFn("sinh", m_sinh);  addFn("cosh", m_cosh);
        addFn("tanh", m_tanh);   addFn("asinh", m_asinh); addFn("acosh", m_acosh);
        addFn("atanh", m_atanh); addFn("degrees", m_degrees);
        addFn("radians", m_radians); addFn("hypot", m_hypot);
        addFn("fmod", m_fmod);   addFn("trunc", m_trunc);
        addFn("is_nan", m_isnan); addFn("is_inf", m_isinf);
        addFn("is_finite", m_isfinite);
        mod->members["pi"] = Value(3.14159265358979323846);
        mod->members["e"] = Value(2.71828182845904523536);
        globals_->define("math", Value(mod));
    }

} // namespace vayu