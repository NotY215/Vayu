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
        Const,   // <destReg> = const <constIndex>
        Nop,
        Ret,     // return <operandReg>
    };

    struct VirInstruction {
        VirOpcode            opcode = VirOpcode::Nop;
        int                  destReg = -1;
        int                  operandReg = -1;
        int                  constIndex = -1;
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