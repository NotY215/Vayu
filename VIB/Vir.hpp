#pragma once
#include "compiler/ast/Ast.hpp"
#include "compiler/lexer/Token.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace vib {

    enum class VirTypeKind { None, Bool, Int, Float, Str, Any };
    struct VirType { VirTypeKind kind = VirTypeKind::None; };

    enum class VirValueKind { None, Bool, Int, Float, Str };
    struct VirValue {
        VirValueKind kind = VirValueKind::None;
        bool         boolValue = false;
        long long    intValue = 0;
        double       floatValue = 0.0;
        std::string  strValue;
    };

    enum class VirOpcode {
        Const,      // <destReg> = const <constIndex>
        Nop,
        Ret,        // return <operandReg>
        // Phase 3.2: arithmetic and comparison.
        Add,        // <destReg> = <lhsReg> + <rhsReg>
        Sub,
        Mul,
        Div,
        Mod,
        Neg,        // <destReg> = -<operandReg>
        CmpEq,      // <destReg> = <lhsReg> == <rhsReg>   (bool)
        CmpNe,
        CmpLt,
        CmpLe,
        CmpGt,
        CmpGe,
        // Phase 3.2: local variable storage.
        StoreLocal, // <localIndex> = <operandReg>
        LoadLocal,  // <destReg> = <localIndex>
        // Phase 4.2: string operations.
        StrConcat,  // <destReg> = <lhsReg> + <rhsReg>  (both must be Str)
        StrEq,      // <destReg> = <lhsReg> == <rhsReg>  (bool; both Str)
        StrNe,      // <destReg> = <lhsReg> != <rhsReg>  (bool; both Str)
        StrCmpLt,   // <destReg> = <lhsReg> <  <rhsReg>  (bool; both Str)
        // Phase 5.1: calls.
        Call,          // <destReg> = functions[funcIndex](argRegs...)  user-defined
        CallBuiltin,   // <destReg> = builtin(argRegs...)                native
        // Phase 6.1: control flow.
        Jump,          // unconditional jump to targetBlock
        JumpIfFalse,   // if !truthy(operandReg) jump to targetBlock
    };

    // Phase 5.1: builtin functions the VIR interpreter can dispatch.
    // Adding entries requires adding an implementation in VirInterp.cpp and
    // a case in the verifier's arity check.
    enum class VirBuiltin {
        Print,          // variadic
        ToStr,          // str(x)       1 arg
        ToInt,          // int(x)       1 arg
        ToFloat,        // float(x)     1 arg
        ToBool,         // bool(x)      1 arg
        LenStr,         // len(s)       1 arg, str only
        // Phase 4.3: string methods, dispatched as builtins with the receiver
        // as argRegs[0].
        StrUpper, StrLower, StrStrip,
        StrContains, StrFind, StrStartsWith, StrEndsWith,
        StrIsDigit, StrIsAlpha, StrIsSpace,
        StrReplace, StrCharAt, StrToInt,
    };

    struct VirInstruction {
        VirOpcode            opcode = VirOpcode::Nop;
        int                  destReg = -1;
        int                  operandReg = -1;
        int                  lhsReg = -1;
        int                  rhsReg = -1;
        int                  constIndex = -1;
        int                  localIndex = -1;
        // Phase 5.1: calls.
        int                  funcIndex = -1;    // Call
        VirBuiltin           builtin = VirBuiltin::Print;   // CallBuiltin
        std::vector<int>     argRegs;           // Call / CallBuiltin
        // Phase 6.1: control flow.
        int                  targetBlock = -1;  // Jump / JumpIfFalse
        vayu::SourceLocation sourceLoc;
    };

    struct VirBlock {
        std::string label;
        std::vector<VirInstruction> instructions;
    };

    struct VirFunction {
        std::string              name;
        std::vector<VirType>     paramTypes;
        std::vector<std::string> paramNames;
        VirType                  returnType;
        int                      localCount = 0;
        std::vector<VirBlock>    blocks;
        int                      entryBlock = 0;
    };

    struct VirModule {
        std::string               name = "main";
        std::vector<VirValue>     constants;
        std::vector<VirFunction>  functions;
        int                       entryFunction = 0;
    };

    // Arithmetic semantics (Phase 3.1 contract):
    //   - int is 64-bit signed two's complement. Overflow wraps (no UB).
    //   - Division by zero raises a VIR runtime error.
    //   - Comparisons return bool.
    //   - Any operator used on mismatched kinds raises a VIR runtime error;
    //     the verifier also rejects obvious static mismatches.
    //   - Unary negation wraps at INT64_MIN (two's complement).
    //
    // String semantics (Phase 4 contract):
    //   - Strings are UTF-8 byte sequences, represented as std::string.
    //   - Concatenation ('+') produces a new string by byte concatenation.
    //   - Equality and ordering are byte-wise (std::string operators).
    //   - Length / indexing / slicing are NOT part of Phase 4 lowering; they
    //     depend on call expressions (Phase 5) and index expressions (Phase 7).
    //   - Method calls (upper / lower / split / ...) are Phase 5 territory.
    //
    // .vye Vayu Executable package — design sketch (Phase 2A.1).
    //
    // A .vye package is a serialized VirModule plus a header and a section
    // directory. It is NOT a native OS executable; a compatible Vayu/Integrator
    // runtime is required to load and execute it. Conceptual layout:
    //
    //   magic          : 4 bytes, "VYE\0"
    //   version        : u16 (currently 0x0001)
    //   flags          : u16 (reserved; 0 for now)
    //   section count  : u16
    //   section table  : (tag u16, offset u32, length u32) * count
    //   section 1 (CONSTANTS): serialized VirValue array (kind tag + payload)
    //   section 2 (FUNCTIONS): serialized VirFunction array (name, paramNames,
    //                          paramTypes, returnType, localCount, blocks,
    //                          instructions)
    //   section 3 (ENTRY):     entry function index (u32) and entry block (u32)
    //   section 4 (DEBUG):     optional; source path, line table (deferred)
    //
    // Deterministic serialization: little-endian, fixed-width integers, string
    // payloads prefixed with u32 length + UTF-8 bytes.
    //
    // VyeWriter::write()   — Phase 2A.2. Deterministic .vye emission.
    // VyeReader::read()    — Phase 2B.1. Strict validation before use:
    //                        rejects bad magic, unknown versions, section
    //                        overlap, out-of-range offsets/lengths, invalid
    //                        constant/function/branch indices, unknown opcodes.
    // VyeRuntime::execute() — Phase 2B.2. Runs a loaded module.
    //
    // Not implemented in this pass. Feature::VyePackaging stays Deferred.
    struct VirPrinter {
        std::string print(const VirModule& module) const;
    };

    struct VerifierResult {
        bool        success = false;
        std::string message;
    };

    struct VirVerifier {
        VerifierResult verify(const VirModule& module) const;
    };

    struct LoweringResult {
        bool        success = false;
        VirModule   module;
        int         plannedPhase = 0;
        std::string failedFeatureName;
        std::string message;
    };

    struct VirLowering {
        LoweringResult lower(const vayu::Block& program) const;
    };

} // namespace vib