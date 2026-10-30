#pragma once
#include "compiler/ast/Ast.hpp"
#include "compiler/lexer/Token.hpp"
#include <string>
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
    };

    struct VirInstruction {
        VirOpcode            opcode = VirOpcode::Nop;
        int                  destReg = -1;
        int                  operandReg = -1;   // Ret / Neg / StoreLocal / LoadLocal source
        int                  lhsReg = -1;       // binary ops
        int                  rhsReg = -1;       // binary ops
        int                  constIndex = -1;   // Const
        int                  localIndex = -1;   // StoreLocal / LoadLocal
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