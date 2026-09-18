#pragma once
#include <cstdint>

namespace vayu {

    /// vayu bytecode instruction set — .
    enum class OpCode : uint8_t {
        // ---- Stack & literals ----
        CONST,          // <index:u16>    push constants[index]
        NONE,           //                push None
        TRUE_V,         //                push true
        FALSE_V,        //                push false
        POP,            //                discard top
        DUP,            //                duplicate top

        // ---- Variables ----
        LOAD,           // <name:u16>     push value of name
        STORE,          // <name:u16>     pop -> assign existing name
        DEFINE,         // <name:u16>     pop -> define-or-reassign name

        // ---- Arithmetic ----
        ADD, SUB, MUL, DIV, FLOORDIV, MOD, POW,
        NEG,            //                unary minus

        // ---- Bitwise (Phase 11.1i) ----
        BAND, BOR, BXOR, SHL, SHR,
        BNOT,           //                unary bitwise NOT (~x)

        // ---- Comparison / logic ----
        EQ, NEQ, LT, GT, LE, GE,
        NOT,

        // ---- Control flow ----
        JUMP,               // <offset:i16>
        JUMP_IF_FALSE,      // <offset:i16>   pop; jump if falsy
        JUMP_IF_TRUE,       // <offset:i16>   pop; jump if truthy

        // ---- Iteration ----
        ITER_NEW,           // pop iterable; push [iterable, idx=0]
        ITER_NEXT,          // <offset:i16>   peek state; if done: pop 2, jump.
        //                else: push next elem, ++idx

        // ---- Lists ----
        LIST_NEW,           // <count:u16>    pop count elems, push list

        // ---- Structs & classes ----
        NEW_INSTANCE,       // <nameIdx:u16> <argc:u8>   pop argc, push instance
        ATTR_GET,           // <nameIdx:u16>              pop base, push attr
        ATTR_SET,           // <nameIdx:u16>              pop value, pop base
        SUPER,              // (no operands)              push super proxy
        // ---- Collections ----
        INDEX_GET,          // [target, index] -> value
        INDEX_SET,          // [target, index, value] -> ()
        MAP_NEW,            // <count:u16>   pop count*2 elems -> map
        // ---- Phase 14.0 / 14.1 ----
        TUPLE_NEW,          // <count:u16>    pop count elems -> tuple
        SET_NEW,            // <count:u16>    pop count elems -> set (dedup'd)
        SLICE,              // [target, start, end] -> sliced container.
        // start or end may be NONE to mean "default".
        IN,                 // [lhs, rhs] -> bool
        // ---- Exceptions ----
        TRY_BEGIN,          // <offset:i16>   push handler
        TRY_END,            //                pop handler (normal exit)
        RAISE,              //                pop value, throw
        RERAISE,            //                throw active exception
        EXCEPT_PUSH,        //                push TOS to activeExceptions_
        EXCEPT_POP,         //                pop activeExceptions_
        EXCEPT_MATCH,       // <nameIdx:u16>  peek TOS, push bool
        // ---- Modules ----
        IMPORT,             // <nameIdx:u16>  load module by name, push module
        IMPORT_MEMBER,      // <nameIdx:u16>  peek module, push .members[name]
        // ---- Functions ----
        MAKE_FN,            // <idx:u16>      push Callable for functions[idx],
        //                closure = current env
        MAKE_GENERATOR,     // <idx:u16>      like MAKE_FN, marks callable as generator
        CALL,               // <argc:u8>      [callee, a0..aN-1] -> result
        RETURN_V,           //                pop value; pop frame; push to caller

        // ---- Generators (Phase 11.1k2) ----
        YIELD_V,            // pop TOS; suspend current generator; on resume

        // ---- Misc ----
        PRINT,              // pop, print with newline
    };

    const char* opCodeName(OpCode op);

} // namespace vayu