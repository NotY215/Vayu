#include "OpCode.hpp"

namespace vayu {

    const char* opCodeName(OpCode op) {
        switch (op) {
        case OpCode::CONST:    return "CONST";
        case OpCode::NONE:     return "NONE";
        case OpCode::TRUE_V:   return "TRUE";
        case OpCode::FALSE_V:  return "FALSE";
        case OpCode::IMPORT:        return "IMPORT";
        case OpCode::IMPORT_MEMBER: return "IMPORT_MEMBER";
        case OpCode::POP:      return "POP";
        case OpCode::DUP:      return "DUP";
        case OpCode::LOAD:     return "LOAD";
        case OpCode::STORE:    return "STORE";
        case OpCode::DEFINE:   return "DEFINE";
        case OpCode::ADD:      return "ADD";
        case OpCode::SUB:      return "SUB";
        case OpCode::MUL:      return "MUL";
        case OpCode::DIV:      return "DIV";
        case OpCode::FLOORDIV: return "FLOORDIV";
        case OpCode::MOD:      return "MOD";
        case OpCode::POW:      return "POW";
        case OpCode::NEG:      return "NEG";
        case OpCode::BAND:     return "BAND";
        case OpCode::BOR:      return "BOR";
        case OpCode::BXOR:     return "BXOR";
        case OpCode::SHL:      return "SHL";
        case OpCode::SHR:      return "SHR";
        case OpCode::BNOT:     return "BNOT";
        case OpCode::EQ:       return "EQ";
        case OpCode::NEQ:      return "NEQ";
        case OpCode::LT:       return "LT";
        case OpCode::GT:       return "GT";
        case OpCode::LE:       return "LE";
        case OpCode::GE:       return "GE";
        case OpCode::NOT:      return "NOT";
        case OpCode::JUMP:           return "JUMP";
        case OpCode::INDEX_GET: return "INDEX_GET";
        case OpCode::INDEX_SET: return "INDEX_SET";
        case OpCode::MAP_NEW:   return "MAP_NEW";
        case OpCode::TUPLE_NEW: return "TUPLE_NEW";
        case OpCode::SET_NEW:   return "SET_NEW";
        case OpCode::SLICE:     return "SLICE";
        case OpCode::IN:        return "IN";
        case OpCode::NEW_INSTANCE: return "NEW_INSTANCE";
        case OpCode::ATTR_GET:     return "ATTR_GET";
        case OpCode::ATTR_SET:     return "ATTR_SET";
        case OpCode::SUPER:        return "SUPER";
        case OpCode::JUMP_IF_FALSE:  return "JUMP_IF_FALSE";
        case OpCode::JUMP_IF_TRUE:   return "JUMP_IF_TRUE";
        case OpCode::ITER_NEW:       return "ITER_NEW";
        case OpCode::ITER_NEXT:      return "ITER_NEXT";
        case OpCode::LIST_NEW:       return "LIST_NEW";
        case OpCode::MAKE_FN:        return "MAKE_FN";
        case OpCode::MAKE_GENERATOR: return "MAKE_GENERATOR";
        case OpCode::CALL:           return "CALL";
        case OpCode::RETURN_V:       return "RETURN_V";
        case OpCode::YIELD_V:        return "YIELD_V";
        case OpCode::PRINT:          return "PRINT";
        case OpCode::TRY_BEGIN:    return "TRY_BEGIN";
        case OpCode::TRY_END:      return "TRY_END";
        case OpCode::RAISE:        return "RAISE";
        case OpCode::RERAISE:      return "RERAISE";
        case OpCode::EXCEPT_PUSH:  return "EXCEPT_PUSH";
        case OpCode::EXCEPT_POP:   return "EXCEPT_POP";
        case OpCode::EXCEPT_MATCH: return "EXCEPT_MATCH";
        }
        return "?";
    }

} // namespace vayu