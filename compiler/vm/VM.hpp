#pragma once
#include "bytecode/Chunk.hpp"
#include "interp/Environment.hpp"
#include "interp/Value.hpp"
#include <deque>
#include <memory>
#include <stdexcept>
#include <vector>

namespace vayu {

    class VMRuntimeError : public std::runtime_error {
    public:
        int line;
        VMRuntimeError(std::string msg, int l)
            : std::runtime_error(std::move(msg)), line(l) {}
    };

    class VM {
    public:
        explicit VM(std::shared_ptr<Environment> globals);
        void run(std::shared_ptr<Chunk> entryChunk);

        Value callVMFunctionSync(std::shared_ptr<Callable> fn,
            const std::vector<Value>& args);

    private:
        struct Handler {
            size_t ip;
            size_t stackSize;
            size_t framesSize;
            size_t activeExcSize;
        };

        struct CallFrame {
            std::shared_ptr<Chunk>       chunk;
            size_t                       ip = 0;
            std::shared_ptr<Environment> env;
            size_t                       stackBase = 0;
            size_t                       handlersAtEntry = 0;
            size_t                       activeExcAtEntry = 0;
        };

        std::shared_ptr<Environment> globals_;
        std::vector<Value>           stack_;
        std::deque<CallFrame>        frames_;    // deque: stable refs across push_back
        std::vector<Handler>         handlers_;
        std::vector<Value>           activeExceptions_;
        std::shared_ptr<GeneratorValue> generatorContext_;
        int                          currentLine_ = 0;

        uint8_t readByte();
        int     readU16();
        int     readI16();

        [[noreturn]] void runtimeError(const std::string& msg);

        void doArithmetic(int opcode);
        void doBitwise(int opcode);
        void doComparison(int opcode);
        void callVMFunction(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args);
        Value createVMGenerator(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args);
        void unwindToHandler(const Value& excValue);

        void runLoop(size_t stopAtFrameCount);
    };

} // namespace vayu