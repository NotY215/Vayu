#pragma once
#include <cstdint>

namespace vayu {

    enum class OpCode : uint8_t {
        CONST, NONE, TRUE_V, FALSE_V, POP, DUP,
        LOAD, STORE, DEFINE,
        ADD, SUB, MUL, DIV, FLOORDIV, MOD, POW, NEG,
        BAND, BOR, BXOR, SHL, SHR, BNOT,
        EQ, NEQ, LT, GT, LE, GE, NOT,
        JUMP, JUMP_IF_FALSE, JUMP_IF_TRUE,
        ITER_NEW, ITER_NEXT,
        LIST_NEW,
        NEW_INSTANCE, ATTR_GET, ATTR_SET, SUPER,
        INDEX_GET, INDEX_SET, MAP_NEW, IN,
        TRY_BEGIN, TRY_END, RAISE, RERAISE,
        EXCEPT_PUSH, EXCEPT_POP, EXCEPT_MATCH,
        IMPORT, IMPORT_MEMBER,
        MAKE_FN, MAKE_GENERATOR, CALL, RETURN_V,
        YIELD_V,
        PRINT,
        TUPLE_NEW, SET_NEW, SLICE,
        ADDR_OF,            // <nameIdx:u16>  reference to a named slot
        DEREF,              // reference -> value
        DEREF_SET,          // value, reference -> store
        // Phase 15.2d — compound lvalue addresses.
        ADDR_OF_INDEX,      // [target, index] -> reference to element
        ADDR_OF_ATTR,       // <nameIdx:u16>  [base] -> reference to field
    };

    const char* opCodeName(OpCode op);

} // namespace vayu