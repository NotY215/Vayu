#include "VM.hpp"
#include "interp/Interpreter.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>

namespace vayu {

    VM::VM(std::shared_ptr<Environment> globals) : globals_(std::move(globals)) {
        stack_.reserve(256);
    }

    // ===========================================================================
    // Low-level readers — kept for call sites outside the hot path.
    // ===========================================================================

    uint8_t VM::readByte() { return frames_.back().chunk->code[frames_.back().ip++]; }
    int VM::readU16() {
        auto& f = frames_.back();
        int v = ((int)f.chunk->code[f.ip] << 8) | (int)f.chunk->code[f.ip + 1];
        f.ip += 2;
        return v;
    }
    int VM::readI16() {
        auto& f = frames_.back();
        int16_t v = (int16_t)(((uint16_t)f.chunk->code[f.ip] << 8) |
            (uint16_t)f.chunk->code[f.ip + 1]);
        f.ip += 2;
        return (int)v;
    }

    [[noreturn]] void VM::runtimeError(const std::string& msg) {
        throw VMRuntimeError(msg, currentLine_);
    }

    // ===========================================================================
    // Forward declaration — defined below, used inside runLoop for IN.
    // ===========================================================================
    static bool valueEqualsVM(const Value& a, const Value& b);

    // ===========================================================================
    // run — installs the VMFunctionRunner hook, then delegates to runLoop
    // ===========================================================================

    void VM::run(std::shared_ptr<Chunk> entryChunk) {
        frames_.clear();
        stack_.clear();
        handlers_.clear();
        activeExceptions_.clear();
        generatorContext_.reset();

        if (Interpreter::current_) {
            Interpreter::current_->setVMFunctionRunner(
                [this](std::shared_ptr<Callable> fn,
                    const std::vector<Value>& args) {
                        return callVMFunctionSync(std::move(fn), args);
                });
        }

        frames_.push_back({ std::move(entryChunk), 0, globals_,
            /*stackBase=*/0, /*handlers=*/0, /*activeExc=*/0 });
        runLoop(0);
    }

    Value VM::callVMFunctionSync(std::shared_ptr<Callable> fn,
        const std::vector<Value>& args) {
        size_t stopAt = frames_.size();
        callVMFunction(fn, args);
        runLoop(stopAt);
        Value v = std::move(stack_.back());
        stack_.pop_back();
        return v;
    }

    // ===========================================================================
    // runLoop — the hot dispatch loop.
    //
    // Top-frame state is cached in locals (chunk / code / constants / ip).
    // The locals are refreshed only when the frame changes (CALL into VM fn,
    // RETURN_V, or an exception unwind).
    // ===========================================================================

    void VM::runLoop(size_t stopAtFrameCount) {
        if (frames_.size() <= stopAtFrameCount) return;

        CallFrame* frame = &frames_.back();
        Chunk* chunk = frame->chunk.get();
        const uint8_t* code = chunk->code.data();
        size_t          codeSize = chunk->code.size();
        const Value* constants = chunk->constants.data();
        size_t          ip = frame->ip;

        auto reload = [&]() {
            frame = &frames_.back();
            chunk = frame->chunk.get();
            code = chunk->code.data();
            codeSize = chunk->code.size();
            constants = chunk->constants.data();
            ip = frame->ip;
            };

        while (frames_.size() > stopAtFrameCount) {
            if (ip >= codeSize)
                runtimeError("bytecode ran off the end without RETURN_V");

            currentLine_ = chunk->lines[ip];
            OpCode op = static_cast<OpCode>(code[ip++]);

            try {
                switch (op) {

                    // ---------- Stack & literals ----------
                case OpCode::CONST: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    stack_.push_back(constants[idx]);
                    break;
                }
                case OpCode::NONE:    stack_.emplace_back();       break;
                case OpCode::TRUE_V:  stack_.emplace_back(true);   break;
                case OpCode::FALSE_V: stack_.emplace_back(false);  break;
                case OpCode::POP:     stack_.pop_back();           break;
                case OpCode::DUP:     stack_.push_back(stack_.back()); break;

                    // ---------- Variables ----------
                case OpCode::LOAD: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& name = chunk->names[idx];
                    Value* slot = frame->env->lookup(name);
                    if (slot) { stack_.push_back(*slot); break; }
                    // Fallback: class / exception constructors.
                    if (Interpreter::current_) {
                        auto cls = Interpreter::current_->vmLookupClass(name);
                        if (cls) {
                            stack_.emplace_back(std::move(cls));
                            break;
                        }
                    }
                    runtimeError("name '" + name + "' is not defined");
                }
                case OpCode::STORE: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& name = chunk->names[idx];
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (!frame->env->assign(name, v))
                        runtimeError("name '" + name + "' is not defined");
                    break;
                }
                case OpCode::DEFINE: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& name = chunk->names[idx];
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (!frame->env->assign(name, v))
                        frame->env->define(name, std::move(v));
                    break;
                }

                                   // ---------- Arithmetic ----------
                case OpCode::ADD:
                case OpCode::SUB:
                case OpCode::MUL:
                case OpCode::DIV:
                case OpCode::FLOORDIV:
                case OpCode::MOD:
                case OpCode::POW:
                    doArithmetic((int)op);
                    break;

                case OpCode::BAND:
                case OpCode::BOR:
                case OpCode::BXOR:
                case OpCode::SHL:
                case OpCode::SHR:
                    doBitwise((int)op);
                    break;

                case OpCode::BNOT: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (v.isInt()) stack_.emplace_back(~v.asInt());
                    else runtimeError("cannot apply '~' to " + v.typeName());
                    break;
                }

                case OpCode::NEG: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (v.isInt())        stack_.emplace_back(-v.asInt());
                    else if (v.isFloat()) stack_.emplace_back(-v.asFloat());
                    else                  runtimeError("cannot negate " + v.typeName());
                    break;
                }

                                // ---------- Comparison / logic ----------
                case OpCode::EQ: case OpCode::NEQ:
                case OpCode::LT: case OpCode::GT:
                case OpCode::LE: case OpCode::GE:
                    doComparison((int)op);
                    break;

                case OpCode::NOT: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    stack_.emplace_back(!v.truthy());
                    break;
                }

                                // ---------- Control flow ----------
                case OpCode::JUMP: {
                    int off = (int)(int16_t)((uint16_t(code[ip]) << 8) |
                        uint16_t(code[ip + 1]));
                    ip += 2;
                    ip = (size_t)((int)ip + off);
                    break;
                }
                case OpCode::JUMP_IF_FALSE: {
                    int off = (int)(int16_t)((uint16_t(code[ip]) << 8) |
                        uint16_t(code[ip + 1]));
                    ip += 2;
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (!v.truthy()) ip = (size_t)((int)ip + off);
                    break;
                }
                case OpCode::JUMP_IF_TRUE: {
                    int off = (int)(int16_t)((uint16_t(code[ip]) << 8) |
                        uint16_t(code[ip + 1]));
                    ip += 2;
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (v.truthy()) ip = (size_t)((int)ip + off);
                    break;
                }

                                         // ---------- Iteration ----------
                case OpCode::ITER_NEW: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (v.isGenerator()) {
                        stack_.push_back(std::move(v));
                        stack_.emplace_back(0LL);
                        break;
                    }
                    if (v.isList()) {
                        stack_.push_back(std::move(v));
                        stack_.emplace_back(0LL);
                        break;
                    }
                    if (v.isMap()) {
                        auto keys = std::make_shared<ListValue>();
                        for (auto& [k, _] : v.asMap()->entries)
                            keys->items.push_back(Value(k));
                        stack_.emplace_back(std::move(keys));
                        stack_.emplace_back(0LL);
                        break;
                    }
                    if (v.isString()) {
                        auto chars = std::make_shared<ListValue>();
                        for (char c : v.asString())
                            chars->items.push_back(Value(std::string(1, c)));
                        stack_.emplace_back(std::move(chars));
                        stack_.emplace_back(0LL);
                        break;
                    }
                    if (v.isTuple()) {
                        auto lst = std::make_shared<ListValue>();
                        lst->items = v.asTuple()->items;
                        stack_.emplace_back(std::move(lst));
                        stack_.emplace_back(0LL);
                        break;
                    }
                    if (v.isSet()) {
                        auto lst = std::make_shared<ListValue>();
                        lst->items = v.asSet()->items;
                        stack_.emplace_back(std::move(lst));
                        stack_.emplace_back(0LL);
                        break;
                    }
                    runtimeError("cannot iterate over " + v.typeName());
                }
                case OpCode::ITER_NEXT: {
                    int off = (int)(int16_t)((uint16_t(code[ip]) << 8) |
                        uint16_t(code[ip + 1]));
                    ip += 2;
                    size_t idxPos = stack_.size() - 1;
                    size_t iterPos = stack_.size() - 2;
                    Value iterable = stack_[iterPos];
                    if (iterable.isGenerator()) {
                        if (!Interpreter::current_)
                            runtimeError("VM: no interpreter context for generator");
                        Value v;
                        bool ok = Interpreter::current_->tryNextGenerator(
                            iterable.asGenerator(), v, SourceLocation{});
                        if (!ok) {
                            ip = (size_t)((int)ip + off);
                            break;
                        }
                        stack_.push_back(std::move(v));
                        break;
                    }
                    long long idx = stack_[idxPos].asInt();
                    if (!iterable.isList()) runtimeError("iterator state corrupted");
                    auto lst = iterable.asList();
                    if (idx < 0 || idx >= (long long)lst->items.size()) {
                        // Do NOT pop here — the compiler emits POP; POP at
                        // the loop exit so `break` uses the same cleanup path.
                        ip = (size_t)((int)ip + off);
                        break;
                    }
                    stack_.push_back(lst->items[(size_t)idx]);
                    stack_[idxPos] = Value(idx + 1);
                    break;
                }

                                      // ---------- Lists ----------
                case OpCode::LIST_NEW: {
                    int count = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    auto lst = std::make_shared<ListValue>();
                    lst->items.resize((size_t)count);
                    for (int i = count - 1; i >= 0; --i) {
                        lst->items[(size_t)i] = std::move(stack_.back());
                        stack_.pop_back();
                    }
                    stack_.emplace_back(std::move(lst));
                    break;
                }
                case OpCode::TUPLE_NEW: {
                    int count = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    auto tup = std::make_shared<TupleValue>();
                    tup->items.resize((size_t)count);
                    for (int i = count - 1; i >= 0; --i) {
                        tup->items[(size_t)i] = std::move(stack_.back());
                        stack_.pop_back();
                    }
                    stack_.emplace_back(std::move(tup));
                    break;
                }
                case OpCode::SET_NEW: {
                    int count = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    auto s = std::make_shared<SetValue>();
                    std::vector<Value> tmp((size_t)count);
                    for (int i = count - 1; i >= 0; --i) {
                        tmp[(size_t)i] = std::move(stack_.back());
                        stack_.pop_back();
                    }
                    for (auto& v : tmp) {
                        bool found = false;
                        for (auto& u : s->items)
                            if (valueEqualsVM(u, v)) { found = true; break; }
                        if (!found) s->items.push_back(std::move(v));
                    }
                    stack_.emplace_back(std::move(s));
                    break;
                }
                case OpCode::ADDR_OF: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& name = chunk->names[idx];
                    auto slot = frame->env->lookupShared(name);
                    if (!slot)
                        runtimeError("cannot take address of '" + name +
                            "' (undefined)");
                    auto ref = std::make_shared<Reference>();
                    ref->accessor = [slot]() -> Value& { return *slot; };
                    stack_.emplace_back(ref);
                    break;
                }
                case OpCode::ADDR_OF_INDEX: {
                    Value idx = std::move(stack_.back()); stack_.pop_back();
                    Value tgt = std::move(stack_.back()); stack_.pop_back();
                    if (tgt.isRef()) {
                        auto ref = tgt.asRef();
                        if (!ref->backing)
                            runtimeError("cannot index a non-arithmetic reference");
                        if (!idx.isInt())
                            runtimeError("pointer index must be int, got " + idx.typeName());
                        long long i = ref->offset + idx.asInt();
                        if (i < 0 || i >= (long long)ref->backing->items.size())
                            runtimeError("pointer index out of range");
                        stack_.push_back(ref->backing->items[(size_t)i]);
                        break;
                    }
                    if (tgt.isList()) {
                        if (!idx.isInt())
                            runtimeError("list index must be int, got " + idx.typeName());
                        auto lst = tgt.asList();
                        long long i = idx.asInt();
                        if (i < 0) i += (long long)lst->items.size();
                        if (i < 0 || i >= (long long)lst->items.size())
                            runtimeError("list index out of range");
                        auto ref = std::make_shared<Reference>();
                        ref->backing = lst;
                        ref->offset = i;
                        ref->accessor = [lst, i]() -> Value& {
                            return lst->items[(size_t)i];
                            };
                        stack_.emplace_back(ref);
                        break;
                    }
                    if (tgt.isMap()) {
                        if (!idx.isString())
                            runtimeError("map key must be str, got " + idx.typeName());
                        auto m = tgt.asMap();
                        std::string k = idx.asString();
                        auto ref = std::make_shared<Reference>();
                        ref->accessor = [m, k]() -> Value& {
                            return m->entries[k];
                            };
                        stack_.emplace_back(ref);
                        break;
                    }
                    runtimeError("cannot take address of this index");
                }
                case OpCode::ADDR_OF_ATTR: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& field = chunk->names[idx];
                    Value base = std::move(stack_.back()); stack_.pop_back();
                    if (!base.isInstance())
                        runtimeError("cannot take address of field on non-object");
                    auto inst = base.asInstance();
                    std::string fn = field;
                    auto ref = std::make_shared<Reference>();
                    ref->accessor = [inst, fn]() -> Value& {
                        return inst->fields[fn];
                        };
                    stack_.emplace_back(ref);
                    break;
                }
                case OpCode::DEREF: {
                    Value p = std::move(stack_.back()); stack_.pop_back();
                    if (p.isRef()) {
                        stack_.push_back(p.asRef()->accessor());
                        break;
                    }
                    if (p.isList() && p.asList()->items.size() == 1) {
                        stack_.push_back(p.asList()->items[0]);
                        break;
                    }
                    runtimeError("'*' expects a reference");
                }
                case OpCode::DEREF_SET: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    Value p = std::move(stack_.back()); stack_.pop_back();
                    if (p.isRef()) {
                        p.asRef()->accessor() = std::move(v);
                        break;
                    }
                    if (p.isList() && p.asList()->items.size() == 1) {
                        p.asList()->items[0] = std::move(v);
                        break;
                    }
                    runtimeError("'*' expects a reference");
                }
                case OpCode::SLICE: {
                    Value endV = std::move(stack_.back()); stack_.pop_back();
                    Value startV = std::move(stack_.back()); stack_.pop_back();
                    Value tgt = std::move(stack_.back()); stack_.pop_back();

                    bool hasStart = !startV.isNone();
                    bool hasEnd = !endV.isNone();
                    long long start = 0, end = 0;

                    auto resolve = [&](const Value& v) -> long long {
                        if (!v.isInt())
                            runtimeError("slice index must be int, got " +
                                v.typeName());
                        return v.asInt();
                        };

                    if (tgt.isList()) {
                        auto lst = tgt.asList();
                        long long len = (long long)lst->items.size();
                        if (hasStart) start = resolve(startV);
                        if (hasEnd)   end = resolve(endV);
                        else          end = len;
                        if (start < 0) start += len;
                        if (end < 0) end += len;
                        if (start < 0) start = 0;
                        if (end < 0) end = 0;
                        if (start > len) start = len;
                        if (end > len) end = len;
                        if (end < start) end = start;
                        auto out = std::make_shared<ListValue>();
                        for (long long i = start; i < end; ++i)
                            out->items.push_back(lst->items[(size_t)i]);
                        stack_.emplace_back(std::move(out));
                        break;
                    }
                    if (tgt.isTuple()) {
                        auto tup = tgt.asTuple();
                        long long len = (long long)tup->items.size();
                        if (hasStart) start = resolve(startV);
                        if (hasEnd)   end = resolve(endV);
                        else          end = len;
                        if (start < 0) start += len;
                        if (end < 0) end += len;
                        if (start < 0) start = 0;
                        if (end < 0) end = 0;
                        if (start > len) start = len;
                        if (end > len) end = len;
                        if (end < start) end = start;
                        auto out = std::make_shared<TupleValue>();
                        for (long long i = start; i < end; ++i)
                            out->items.push_back(tup->items[(size_t)i]);
                        stack_.emplace_back(std::move(out));
                        break;
                    }
                    if (tgt.isString()) {
                        const std::string& s = tgt.asString();
                        long long len = (long long)s.size();
                        if (hasStart) start = resolve(startV);
                        if (hasEnd)   end = resolve(endV);
                        else          end = len;
                        if (start < 0) start += len;
                        if (end < 0) end += len;
                        if (start < 0) start = 0;
                        if (end < 0) end = 0;
                        if (start > len) start = len;
                        if (end > len) end = len;
                        if (end < start) end = start;
                        stack_.emplace_back(
                            s.substr((size_t)start, (size_t)(end - start)));
                        break;
                    }
                    runtimeError("cannot slice value of type " + tgt.typeName());
                }

                                     // ---------- Functions ----------
                case OpCode::MAKE_FN:
                case OpCode::MAKE_GENERATOR: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    auto& fnChunk = chunk->functions[(size_t)idx];
                    auto c = std::make_shared<Callable>();
                    c->kind = Callable::Kind::VMFunction;
                    c->name = (op == OpCode::MAKE_GENERATOR) ? "<gen>" : "<fn>";
                    c->chunk = fnChunk;
                    c->vmParams = fnChunk->paramNames;
                    c->closure = frame->env;
                    c->isGenerator = (op == OpCode::MAKE_GENERATOR);
                    stack_.emplace_back(std::move(c));
                    break;
                }

                case OpCode::CALL: {
                    int argc = (int)code[ip++];
                    std::vector<Value> args((size_t)argc);
                    for (int i = argc - 1; i >= 0; --i) {
                        args[(size_t)i] = std::move(stack_.back());
                        stack_.pop_back();
                    }
                    Value callee = std::move(stack_.back()); stack_.pop_back();

                    if (!callee.isCallable())
                        runtimeError("cannot call " + callee.typeName() + " value");

                    auto fn = callee.asCallable();
                    if (fn->kind == Callable::Kind::VMFunction) {
                        if (fn->isGenerator) {
                            Value gen = createVMGenerator(fn, args);
                            stack_.push_back(std::move(gen));
                        }
                        else {
                            frame->ip = ip;
                            callVMFunction(fn, args);
                            reload();
                        }
                    }
                    else {
                        if (!Interpreter::current_)
                            runtimeError("VM: no interpreter context for builtin call");
                        Value r = Interpreter::current_->callValue(
                            callee, args, SourceLocation{});
                        stack_.push_back(std::move(r));
                    }
                    break;
                }

                case OpCode::YIELD_V: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    auto gen = generatorContext_;
                    if (!gen) runtimeError("'yield' outside a generator");

                    std::unique_lock<std::mutex> lk(gen->mtx);
                    gen->yielded = std::move(v);
                    gen->state = GenState::Suspended;
                    gen->cv.notify_all();
                    gen->cv.wait(lk, [&] { return gen->resume || gen->cancel; });
                    if (gen->cancel) throw GeneratorCancelled{};
                    gen->resume = false;
                    gen->state = GenState::Running;
                    break;
                }

                case OpCode::RETURN_V: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    size_t sBase = frame->stackBase;
                    size_t hAtEntry = frame->handlersAtEntry;
                    size_t aAtEntry = frame->activeExcAtEntry;
                    frames_.pop_back();
                    if (stack_.size() > sBase) stack_.resize(sBase);
                    if (handlers_.size() > hAtEntry) handlers_.resize(hAtEntry);
                    if (activeExceptions_.size() > aAtEntry)
                        activeExceptions_.resize(aAtEntry);
                    stack_.push_back(std::move(v));
                    if (frames_.size() <= stopAtFrameCount) return;
                    reload();
                    break;
                }

                                     // ---------- Structs & classes ----------
                case OpCode::NEW_INSTANCE: {
                    int nameIdx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    int argc = (int)code[ip++];
                    const std::string& clsName = chunk->names[nameIdx];
                    std::vector<Value> args((size_t)argc);
                    for (int i = argc - 1; i >= 0; --i) {
                        args[(size_t)i] = std::move(stack_.back());
                        stack_.pop_back();
                    }
                    if (!Interpreter::current_)
                        runtimeError("VM: no interpreter context for instance construction");
                    Value inst = Interpreter::current_->vmNewInst(
                        clsName, args, SourceLocation{});
                    stack_.push_back(std::move(inst));
                    break;
                }
                case OpCode::ATTR_GET: {
                    int nameIdx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& attr = chunk->names[nameIdx];
                    Value base = std::move(stack_.back()); stack_.pop_back();
                    if (!Interpreter::current_)
                        runtimeError("VM: no interpreter context for attribute lookup");
                    Value v = Interpreter::current_->vmGetAttr(
                        base, attr, SourceLocation{});
                    stack_.push_back(std::move(v));
                    break;
                }
                case OpCode::ATTR_SET: {
                    int nameIdx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& attr = chunk->names[nameIdx];
                    Value value = std::move(stack_.back()); stack_.pop_back();
                    Value base = std::move(stack_.back()); stack_.pop_back();
                    if (!Interpreter::current_)
                        runtimeError("VM: no interpreter context for attribute write");
                    Interpreter::current_->vmSetAttr(base, attr, value, SourceLocation{});
                    break;
                }
                case OpCode::SUPER: {
                    Value* s = frame->env->lookup("self");
                    Value* k = frame->env->lookup("__class__");
                    if (!s || !k || !s->isInstance() || !k->isClass())
                        runtimeError("super() outside method");
                    auto sup = std::make_shared<Callable>();
                    sup->kind = Callable::Kind::SuperMethod;
                    sup->name = "super";
                    sup->boundSelf = s->asInstance();
                    sup->superParent = k->asClass()->parent;
                    if (!sup->superParent)
                        runtimeError("class '" + k->asClass()->name + "' has no parent");
                    stack_.emplace_back(std::move(sup));
                    break;
                }

                                  // ---------- Collections ----------
                case OpCode::INDEX_GET: {
                    Value idx = std::move(stack_.back()); stack_.pop_back();
                    Value tgt = std::move(stack_.back()); stack_.pop_back();
                    if (tgt.isRef()) {
                        auto ref = tgt.asRef();
                        if (!ref->backing)
                            runtimeError("cannot index a non-arithmetic reference");
                        if (!idx.isInt())
                            runtimeError("pointer index must be int, got " + idx.typeName());
                        long long i = ref->offset + idx.asInt();
                        if (i < 0 || i >= (long long)ref->backing->items.size())
                            runtimeError("pointer index out of range");
                        stack_.push_back(ref->backing->items[(size_t)i]);
                        break;
                    }
                    if (tgt.isList()) {
                        if (!idx.isInt())
                            runtimeError("list index must be int, got " + idx.typeName());
                        auto lst = tgt.asList();
                        long long i = idx.asInt();
                        if (i < 0) i += (long long)lst->items.size();
                        if (i < 0 || i >= (long long)lst->items.size())
                            runtimeError("list index out of range");
                        stack_.push_back(lst->items[(size_t)i]);
                        break;
                    }
                    if (tgt.isMap()) {
                        if (!idx.isString())
                            runtimeError("map key must be str, got " + idx.typeName());
                        auto m = tgt.asMap();
                        auto it = m->entries.find(idx.asString());
                        if (it == m->entries.end())
                            runtimeError("map has no key '" + idx.asString() + "'");
                        stack_.push_back(it->second);
                        break;
                    }
                    if (tgt.isString()) {
                        if (!idx.isInt())
                            runtimeError("str index must be int, got " + idx.typeName());
                        const std::string& s = tgt.asString();
                        long long i = idx.asInt();
                        if (i < 0) i += (long long)s.size();
                        if (i < 0 || i >= (long long)s.size())
                            runtimeError("string index out of range");
                        stack_.emplace_back(std::string(1, s[(size_t)i]));
                        break;
                    }
                    if (tgt.isTuple()) {
                        if (!idx.isInt())
                            runtimeError("tuple index must be int, got " + idx.typeName());
                        const auto& items = tgt.asTuple()->items;
                        long long i = idx.asInt();
                        if (i < 0) i += (long long)items.size();
                        if (i < 0 || i >= (long long)items.size())
                            runtimeError("tuple index out of range");
                        stack_.push_back(items[(size_t)i]);
                        break;
                    }
                    runtimeError("cannot index value of type " + tgt.typeName());
                }
                case OpCode::INDEX_SET: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    Value idx = std::move(stack_.back()); stack_.pop_back();
                    Value tgt = std::move(stack_.back()); stack_.pop_back();
                    if (tgt.isRef()) {
                        auto ref = tgt.asRef();
                        if (!ref->backing)
                            runtimeError("cannot index a non-arithmetic reference");
                        if (!idx.isInt())
                            runtimeError("pointer index must be int, got " + idx.typeName());
                        long long i = ref->offset + idx.asInt();
                        if (i < 0 || i >= (long long)ref->backing->items.size())
                            runtimeError("pointer index out of range");
                        ref->backing->items[(size_t)i] = std::move(v);
                        break;
                    }
                    if (tgt.isList()) {
                        if (!idx.isInt())
                            runtimeError("list index must be int, got " + idx.typeName());
                        auto lst = tgt.asList();
                        long long i = idx.asInt();
                        if (i < 0) i += (long long)lst->items.size();
                        if (i < 0 || i >= (long long)lst->items.size())
                            runtimeError("list index out of range");
                        lst->items[(size_t)i] = std::move(v);
                        break;
                    }
                    if (tgt.isMap()) {
                        if (!idx.isString())
                            runtimeError("map key must be str, got " + idx.typeName());
                        tgt.asMap()->entries[idx.asString()] = std::move(v);
                        break;
                    }
                    if (tgt.isString())
                        runtimeError("strings are immutable");
                    runtimeError("cannot index-assign to value of type " + tgt.typeName());
                }
                case OpCode::MAP_NEW: {
                    int count = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    auto m = std::make_shared<MapValue>();
                    std::vector<std::pair<std::string, Value>> pairs((size_t)count);
                    for (int i = count - 1; i >= 0; --i) {
                        Value v = std::move(stack_.back()); stack_.pop_back();
                        Value k = std::move(stack_.back()); stack_.pop_back();
                        if (!k.isString())
                            runtimeError("map keys must be str, got " + k.typeName());
                        pairs[(size_t)i] = { k.asString(), std::move(v) };
                    }
                    for (auto& [k, v] : pairs) m->entries[k] = std::move(v);
                    stack_.emplace_back(std::move(m));
                    break;
                }
                case OpCode::IN: {
                    Value r = std::move(stack_.back()); stack_.pop_back();
                    Value l = std::move(stack_.back()); stack_.pop_back();
                    if (r.isList()) {
                        bool found = false;
                        for (auto& v : r.asList()->items)
                            if (valueEqualsVM(l, v)) { found = true; break; }
                        stack_.emplace_back(found);
                        break;
                    }
                    if (r.isSet()) {
                        bool found = false;
                        for (auto& v : r.asSet()->items)
                            if (valueEqualsVM(l, v)) { found = true; break; }
                        stack_.emplace_back(found);
                        break;
                    }
                    if (r.isTuple()) {
                        bool found = false;
                        for (auto& v : r.asTuple()->items)
                            if (valueEqualsVM(l, v)) { found = true; break; }
                        stack_.emplace_back(found);
                        break;
                    }
                    if (r.isMap()) {
                        if (!l.isString()) { stack_.emplace_back(false); break; }
                        stack_.emplace_back(r.asMap()->entries.count(l.asString()) > 0);
                        break;
                    }
                    if (r.isString() && l.isString()) {
                        stack_.emplace_back(r.asString().find(l.asString()) != std::string::npos);
                        break;
                    }
                    runtimeError("'in' requires a container on the right");
                }

                               // ---------- Exceptions ----------
                case OpCode::TRY_BEGIN: {
                    int off = (int)(int16_t)((uint16_t(code[ip]) << 8) |
                        uint16_t(code[ip + 1]));
                    ip += 2;
                    Handler h;
                    h.ip = (size_t)((int)ip + off);
                    h.stackSize = stack_.size();
                    h.framesSize = frames_.size();
                    h.activeExcSize = activeExceptions_.size();
                    handlers_.push_back(h);
                    break;
                }
                case OpCode::TRY_END:
                    if (!handlers_.empty()) handlers_.pop_back();
                    break;

                case OpCode::RAISE: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    if (!v.isInstance()) {
                        if (!Interpreter::current_)
                            runtimeError("VM: no interpreter context for exception creation");
                        v = Interpreter::current_->vmMakeException("Exception", v.toString());
                    }
                    throw VayuException{ v, SourceLocation{} };
                }
                case OpCode::RERAISE: {
                    if (activeExceptions_.empty())
                        runtimeError("no active exception to re-raise");
                    throw VayuException{ activeExceptions_.back(), SourceLocation{} };
                }
                case OpCode::EXCEPT_PUSH: {
                    if (stack_.empty()) runtimeError("EXCEPT_PUSH: empty stack");
                    activeExceptions_.push_back(stack_.back());
                    break;
                }
                case OpCode::EXCEPT_POP:
                    if (!activeExceptions_.empty()) activeExceptions_.pop_back();
                    break;
                case OpCode::EXCEPT_MATCH: {
                    int nameIdx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& clsName = chunk->names[nameIdx];
                    if (stack_.empty()) runtimeError("EXCEPT_MATCH: empty stack");
                    if (!Interpreter::current_)
                        runtimeError("VM: no interpreter context for exception match");
                    bool match = Interpreter::current_->vmIsInstanceOf(stack_.back(), clsName);
                    stack_.emplace_back(match);
                    break;
                }

                                         // ---------- Modules ----------
                case OpCode::IMPORT: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& modName = chunk->names[idx];
                    if (!Interpreter::current_)
                        runtimeError("VM: no interpreter context for module load");
                    Value mod = Interpreter::current_->vmLoadModule(
                        modName, SourceLocation{});
                    stack_.push_back(std::move(mod));
                    break;
                }
                case OpCode::IMPORT_MEMBER: {
                    int idx = (int(code[ip]) << 8) | int(code[ip + 1]); ip += 2;
                    const std::string& member = chunk->names[idx];
                    if (stack_.empty() || !stack_.back().isModule())
                        runtimeError("IMPORT_MEMBER: top of stack is not a module");
                    auto mod = stack_.back().asModule();
                    auto it = mod->members.find(member);
                    if (it == mod->members.end())
                        runtimeError("module '" + mod->name +
                            "' has no member '" + member + "'");
                    stack_.push_back(it->second);
                    break;
                }

                                          // ---------- Misc ----------
                case OpCode::PRINT: {
                    Value v = std::move(stack_.back()); stack_.pop_back();
                    std::cout << v.toString() << '\n';
                    break;
                }
                }
            }
            catch (VayuException& e) {
                if (!frames_.empty() && &frames_.back() == frame)
                    frame->ip = ip;
                unwindToHandler(e.value);
                if (frames_.size() <= stopAtFrameCount) throw;
                reload();
            }
            catch (const RuntimeError& e) {
                if (!frames_.empty() && &frames_.back() == frame)
                    frame->ip = ip;
                Value exc;
                if (Interpreter::current_)
                    exc = Interpreter::current_->vmMakeException(
                        "RuntimeError", e.what());
                else
                    exc = Value(e.what());
                unwindToHandler(exc);
                if (frames_.size() <= stopAtFrameCount)
                    throw VayuException{ exc, e.loc };
                reload();
            }
        }
    }

    // ===========================================================================
    // Exception unwinding
    // ===========================================================================

    void VM::unwindToHandler(const Value& excValue) {
        if (handlers_.empty())
            throw VayuException{ excValue, SourceLocation{} };

        Handler h = handlers_.back();
        handlers_.pop_back();

        if (stack_.size() > h.stackSize)             stack_.resize(h.stackSize);
        if (frames_.size() > h.framesSize)           frames_.resize(h.framesSize);
        if (activeExceptions_.size() > h.activeExcSize)
            activeExceptions_.resize(h.activeExcSize);

        stack_.push_back(excValue);
        frames_.back().ip = h.ip;
    }

    // ===========================================================================
    // Function calls
    // ===========================================================================

    void VM::callVMFunction(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args) {
        if (args.size() != fn->vmParams.size())
            runtimeError("function expects " + std::to_string(fn->vmParams.size()) +
                " argument(s), got " + std::to_string(args.size()));

        auto callEnv = std::make_shared<Environment>(
            fn->closure ? fn->closure : globals_);
        for (size_t i = 0; i < args.size(); ++i)
            callEnv->define(fn->vmParams[i], args[i]);

        frames_.push_back({ fn->chunk, 0, callEnv,
                           stack_.size(),
                           handlers_.size(), activeExceptions_.size() });
    }

    // ===========================================================================
// Generators — spawn a fresh VM per generator.
//
// Each generator runs on its own thread with its own VM instance, so the
// owner's frames_/stack_/handlers_ and the generator's never collide.
// The owner blocks while the generator runs; the generator blocks at
// YIELD_V while the owner runs.  Sequential access, no locks on VM state.
// ===========================================================================
    Value VM::createVMGenerator(const std::shared_ptr<Callable>& fn,
        const std::vector<Value>& args) {
        auto gen = std::make_shared<GeneratorValue>();
        auto globals = globals_;
        auto fnChunk = fn->chunk;
        auto fnParams = fn->vmParams;
        auto closure = fn->closure;
        Interpreter* interp = Interpreter::current_;

        gen->worker = std::thread([gen, fnChunk, fnParams, closure,
            globals, args, interp]() {
                Interpreter::current_ = interp;

                VM vm(globals);

                // Wait for the first resume.
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

                auto callEnv = std::make_shared<Environment>(
                    closure ? closure : globals);
                for (size_t i = 0; i < args.size(); ++i)
                    callEnv->define(fnParams[i], args[i]);

                vm.generatorContext_ = gen;
                vm.frames_.push_back({ fnChunk, 0, callEnv, 0, 0, 0 });

                try {
                    vm.runLoop(0);
                }
                catch (GeneratorCancelled&) {
                    // Owner cancelled — drop out.
                }
                catch (...) {
                    std::lock_guard<std::mutex> lk(gen->mtx);
                    gen->pendingError = std::current_exception();
                }

                std::lock_guard<std::mutex> lk(gen->mtx);
                gen->state = GenState::Done;
                gen->cv.notify_all();
            });

        return Value(gen);
    }

    // ===========================================================================
    // Arithmetic
    // ===========================================================================

    void VM::doArithmetic(int opcode) {
        Value r = std::move(stack_.back()); stack_.pop_back();
        Value l = std::move(stack_.back()); stack_.pop_back();
        OpCode op = static_cast<OpCode>(opcode);

        switch (op) {
        case OpCode::ADD:
            if (l.isInt() && r.isInt()) { stack_.emplace_back(l.asInt() + r.asInt());         return; }
            if (l.isNumber() && r.isNumber()) { stack_.emplace_back(l.asDouble() + r.asDouble());   return; }
            if (l.isString() && r.isString()) { stack_.emplace_back(l.asString() + r.asString());   return; }
            if (l.isList() && r.isList()) {
                auto out = std::make_shared<ListValue>();
                out->items = l.asList()->items;
                for (auto& v : r.asList()->items) out->items.push_back(v);
                stack_.emplace_back(std::move(out));
                return;
            }
            if (l.isTuple() && r.isTuple()) {
                auto out = std::make_shared<TupleValue>();
                out->items = l.asTuple()->items;
                for (auto& v : r.asTuple()->items) out->items.push_back(v);
                stack_.emplace_back(std::move(out));
                return;
            }
            if (l.isRef() && r.isInt()) {
                auto ref = l.asRef();
                if (!ref->backing)
                    runtimeError("cannot perform pointer arithmetic on this reference");
                long long n = r.asInt();
                auto out = std::make_shared<Reference>();
                out->backing = ref->backing;
                out->offset = ref->offset + n;
                auto lst = ref->backing;
                long long off = out->offset;
                out->accessor = [lst, off]() -> Value& {
                    if (off < 0 || off >= (long long)lst->items.size())
                        throw std::runtime_error("pointer out of range");
                    return lst->items[(size_t)off];
                    };
                stack_.emplace_back(out);
                return;
            }
            runtimeError("cannot add " + l.typeName() + " and " + r.typeName());

        case OpCode::SUB:
            if (l.isInt() && r.isInt()) { stack_.emplace_back(l.asInt() - r.asInt());         return; }
            if (l.isNumber() && r.isNumber()) { stack_.emplace_back(l.asDouble() - r.asDouble());   return; }
            runtimeError("cannot subtract " + r.typeName() + " from " + l.typeName());

        case OpCode::MUL: {
            if (l.isInt() && r.isInt()) { stack_.emplace_back(l.asInt() * r.asInt());         return; }
            if (l.isNumber() && r.isNumber()) { stack_.emplace_back(l.asDouble() * r.asDouble());   return; }
            if (l.isString() && r.isInt()) {
                std::string o; for (long long i = 0; i < r.asInt(); ++i) o += l.asString();
                stack_.emplace_back(std::move(o)); return;
            }
            if (l.isInt() && r.isString()) {
                std::string o; for (long long i = 0; i < l.asInt(); ++i) o += r.asString();
                stack_.emplace_back(std::move(o)); return;
            }
            if (l.isList() && r.isInt()) {
                auto out = std::make_shared<ListValue>();
                for (long long i = 0; i < r.asInt(); ++i)
                    for (auto& v : l.asList()->items) out->items.push_back(v);
                stack_.emplace_back(std::move(out)); return;
            }
            if (l.isInt() && r.isList()) {
                auto out = std::make_shared<ListValue>();
                for (long long i = 0; i < l.asInt(); ++i)
                    for (auto& v : r.asList()->items) out->items.push_back(v);
                stack_.emplace_back(std::move(out)); return;
            }
            runtimeError("cannot multiply " + l.typeName() + " by " + r.typeName());
        }

        case OpCode::DIV: {
            if (l.isNumber() && r.isNumber()) {
                double d = r.asDouble();
                if (d == 0.0) runtimeError("division by zero");
                stack_.emplace_back(l.asDouble() / d); return;
            }
            runtimeError("cannot divide " + l.typeName() + " by " + r.typeName());
        }

        case OpCode::FLOORDIV: {
            if (l.isNumber() && r.isNumber()) {
                double d = r.asDouble();
                if (d == 0.0) runtimeError("division by zero");
                double q = std::floor(l.asDouble() / d);
                if (l.isInt() && r.isInt()) stack_.emplace_back((long long)q);
                else                        stack_.emplace_back(q);
                return;
            }
            runtimeError("cannot apply '//' to " + l.typeName() + " and " + r.typeName());
        }

        case OpCode::MOD: {
            if (l.isNumber() && r.isNumber()) {
                double d = r.asDouble();
                if (d == 0.0) runtimeError("modulo by zero");
                double m = std::fmod(l.asDouble(), d);
                if (m != 0 && ((m < 0) != (d < 0))) m += d;
                if (l.isInt() && r.isInt()) stack_.emplace_back((long long)m);
                else                        stack_.emplace_back(m);
                return;
            }
            runtimeError("cannot apply '%' to " + l.typeName() + " and " + r.typeName());
        }

        case OpCode::POW: {
            if (l.isNumber() && r.isNumber()) {
                if (l.isInt() && r.isInt() && r.asInt() >= 0) {
                    long long base = l.asInt(), exp = r.asInt(), acc = 1;
                    while (exp--) acc *= base;
                    stack_.emplace_back(acc); return;
                }
                stack_.emplace_back(std::pow(l.asDouble(), r.asDouble())); return;
            }
            runtimeError("cannot apply '**' to " + l.typeName() + " and " + r.typeName());
        }

        default: runtimeError("internal: not an arithmetic op");
        }
    }

    // ===========================================================================
    // Bitwise
    // ===========================================================================

    void VM::doBitwise(int opcode) {
        Value r = std::move(stack_.back()); stack_.pop_back();
        Value l = std::move(stack_.back()); stack_.pop_back();
        if (!l.isInt() || !r.isInt())
            runtimeError(std::string("bitwise op requires int operands (got ") +
                l.typeName() + " and " + r.typeName() + ")");
        long long a = l.asInt(), b = r.asInt();
        switch (static_cast<OpCode>(opcode)) {
        case OpCode::BAND: stack_.emplace_back(a & b); return;
        case OpCode::BOR:  stack_.emplace_back(a | b); return;
        case OpCode::BXOR: stack_.emplace_back(a ^ b); return;
        case OpCode::SHL:  stack_.emplace_back(a << b); return;
        case OpCode::SHR:  stack_.emplace_back(a >> b); return;
        default: runtimeError("internal: not a bitwise op");
        }
    }

    // ===========================================================================
    // Equality helper (used by IN, and by doComparison)
    // ===========================================================================

    static bool valueEqualsVM(const Value& a, const Value& b) {
        if (a.isNumber() && b.isNumber()) return a.asDouble() == b.asDouble();
        if (a.isString() && b.isString()) return a.asString() == b.asString();
        if (a.isBool() && b.isBool())   return a.asBool() == b.asBool();
        if (a.isNone() && b.isNone())   return true;
        if (a.isInstance() && b.isInstance()) return a.asInstance() == b.asInstance();
        if (a.isList() && b.isList()) return a.asList() == b.asList();
        if (a.isMap() && b.isMap())  return a.asMap() == b.asMap();
        if (a.isTuple() && b.isTuple()) {
            const auto& x = a.asTuple()->items;
            const auto& y = b.asTuple()->items;
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.size(); ++i)
                if (!valueEqualsVM(x[i], y[i])) return false;
            return true;
        }
        if (a.isSet() && b.isSet()) {
            const auto& x = a.asSet()->items;
            const auto& y = b.asSet()->items;
            if (x.size() != y.size()) return false;
            for (auto& xv : x) {
                bool found = false;
                for (auto& yv : y) if (valueEqualsVM(xv, yv)) { found = true; break; }
                if (!found) return false;
            }
            return true;
        }
        return false;
    }

    // ===========================================================================
    // Comparison
    // ===========================================================================

    void VM::doComparison(int opcode) {
        Value r = std::move(stack_.back()); stack_.pop_back();
        Value l = std::move(stack_.back()); stack_.pop_back();
        OpCode op = static_cast<OpCode>(opcode);

        if (op == OpCode::EQ) { stack_.emplace_back(valueEqualsVM(l, r)); return; }
        if (op == OpCode::NEQ) { stack_.emplace_back(!valueEqualsVM(l, r)); return; }

        bool res = false;
        if (l.isNumber() && r.isNumber()) {
            double a = l.asDouble(), b = r.asDouble();
            switch (op) {
            case OpCode::LT: res = a < b; break;
            case OpCode::GT: res = a > b; break;
            case OpCode::LE: res = a <= b; break;
            case OpCode::GE: res = a >= b; break;
            default: break;
            }
        }
        else if (l.isString() && r.isString()) {
            const auto& a = l.asString(); const auto& b = r.asString();
            switch (op) {
            case OpCode::LT: res = a < b; break;
            case OpCode::GT: res = a > b; break;
            case OpCode::LE: res = a <= b; break;
            case OpCode::GE: res = a >= b; break;
            default: break;
            }
        }
        else {
            runtimeError("cannot compare " + l.typeName() + " and " + r.typeName());
        }
        stack_.emplace_back(res);
    }

} // namespace vayu