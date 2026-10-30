#include "Vir.hpp"
#include <sstream>

namespace vib {

    namespace {

        const char* typeName(VirTypeKind k) {
            switch (k) {
            case VirTypeKind::None:  return "None";
            case VirTypeKind::Bool:  return "bool";
            case VirTypeKind::Int:   return "int";
            case VirTypeKind::Float: return "float";
            case VirTypeKind::Str:   return "str";
            case VirTypeKind::Any:   return "any";
            }
            return "?";
        }

        std::string printValue(const VirValue& v) {
            switch (v.kind) {
            case VirValueKind::None:  return "None";
            case VirValueKind::Bool:  return v.boolValue ? "true" : "false";
            case VirValueKind::Int:   return std::to_string(v.intValue);
            case VirValueKind::Float: {
                std::ostringstream oss;
                oss.precision(15);
                oss << v.floatValue;
                return oss.str();
            }
            case VirValueKind::Str:   return "\"" + v.strValue + "\"";
            }
            return "?";
        }

        std::string printInstruction(const VirInstruction& i) {
            switch (i.opcode) {
            case VirOpcode::Const:
                return "r" + std::to_string(i.destReg) + " = Const([" +
                    std::to_string(i.constIndex) + "])";
            case VirOpcode::Nop:
                return "Nop";
            case VirOpcode::Ret:
                return "Ret(r" + std::to_string(i.operandReg) + ")";
            }
            return "?";
        }

        bool isTerminator(VirOpcode op) {
            return op == VirOpcode::Ret;
        }

        // Map a StmtKind to the feature that will eventually lower it, and the
        // roadmap phase that will land it. Pass / Block are handled inline.
        struct BlockerInfo {
            const char* featureName;
            int         phase;
            const char* stmtKindName;
        };

        BlockerInfo blockerFor(const vayu::Stmt* s) {
            using SK = vayu::StmtKind;
            switch (s->kind) {
            case SK::Expr:              return { "BasicArithmetic",  3, "ExprStmt" };
            case SK::Assign:            return { "BasicArithmetic",  3, "Assign" };
            case SK::AnnotAssign:       return { "BasicArithmetic",  3, "AnnotAssign" };
            case SK::Const:             return { "BasicArithmetic",  3, "Const" };
            case SK::Enum:              return { "BasicArithmetic",  3, "Enum" };
            case SK::If:                return { "ControlFlow",      6, "If" };
            case SK::While:             return { "ControlFlow",      6, "While" };
            case SK::For:               return { "ControlFlow",      6, "For" };
            case SK::Break:             return { "ControlFlow",      6, "Break" };
            case SK::Continue:          return { "ControlFlow",      6, "Continue" };
            case SK::Def:               return { "Functions",        5, "Def" };
            case SK::Return:            return { "Functions",        5, "Return" };
            case SK::Class:             return { "Classes",         10, "Class" };
            case SK::Struct:            return { "Classes",         10, "Struct" };
            case SK::Try:               return { "Exceptions",      10, "Try" };
            case SK::Raise:             return { "Exceptions",      10, "Raise" };
            case SK::Yield:             return { "Generators",      10, "Yield" };
            case SK::Import:            return { "Modules",          8, "Import" };
            case SK::FromImport:        return { "Modules",          8, "FromImport" };
            case SK::Extern:            return { "RuntimeEmission", 12, "Extern" };
            case SK::Pass:              return { "VIR",              0, "Pass" };
            case SK::Block:             return { "VIR",              0, "Block" };
            }
            return { "VIR", 0, "?" };
        }

        // Recursive lowering of a Block into the current function's entry block.
        // Returns true on success; on failure, fills `result` and returns false.
        bool lowerBlock(const vayu::Block& block,
            VirFunction& fn,
            VirBlock& entry,
            LoweringResult& result) {
            for (const auto& stmtPtr : block.stmts) {
                const vayu::Stmt* s = stmtPtr.get();
                if (s->kind == vayu::StmtKind::Pass) {
                    continue;   // no-op
                }
                if (s->kind == vayu::StmtKind::Block) {
                    const auto* bs = static_cast<const vayu::BlockStmt*>(s);
                    if (!lowerBlock(bs->body, fn, entry, result)) return false;
                    continue;
                }
                BlockerInfo b = blockerFor(s);
                result.success = false;
                result.plannedPhase = b.phase;
                result.failedFeatureName = b.featureName;
                result.message = std::string("lowering for ") + b.stmtKindName +
                    " is not implemented yet (planned Phase " +
                    std::to_string(b.phase) + ")";
                return false;
            }
            return true;
        }

    } // namespace

    // ============================================================================
    // Printer
    // ============================================================================

    std::string VirPrinter::print(const VirModule& module) const {
        std::ostringstream oss;
        oss << "module \"" << module.name << "\" {\n";
        oss << "  constants:\n";
        for (size_t i = 0; i < module.constants.size(); ++i) {
            oss << "    [" << i << "] " << printValue(module.constants[i]) << "\n";
        }
        for (const auto& fn : module.functions) {
            oss << "  function " << fn.name << "(";
            for (size_t i = 0; i < fn.paramNames.size(); ++i) {
                if (i) oss << ", ";
                oss << fn.paramNames[i] << ": " << typeName(fn.paramTypes[i].kind);
            }
            oss << ") -> " << typeName(fn.returnType.kind) << " {\n";
            oss << "    locals: " << fn.localCount << "\n";
            for (const auto& block : fn.blocks) {
                oss << "    " << block.label << ":\n";
                for (const auto& instr : block.instructions) {
                    oss << "      " << printInstruction(instr) << "\n";
                }
            }
            oss << "  }\n";
        }
        oss << "}\n";
        return oss.str();
    }

    // ============================================================================
    // Verifier — structural checks only in Phase 2.
    // ============================================================================

    VerifierResult VirVerifier::verify(const VirModule& module) const {
        VerifierResult r;
        if (module.entryFunction < 0 ||
            module.entryFunction >= (int)module.functions.size()) {
            r.message = "verifier: entryFunction index " +
                std::to_string(module.entryFunction) +
                " out of range (module has " +
                std::to_string(module.functions.size()) + " functions)";
            return r;
        }
        for (size_t fi = 0; fi < module.functions.size(); ++fi) {
            const auto& fn = module.functions[fi];
            if (fn.blocks.empty()) {
                r.message = "verifier: function \"" + fn.name + "\" has no blocks";
                return r;
            }
            if (fn.entryBlock < 0 || fn.entryBlock >= (int)fn.blocks.size()) {
                r.message = "verifier: function \"" + fn.name +
                    "\" entryBlock " + std::to_string(fn.entryBlock) +
                    " out of range";
                return r;
            }
            if (fn.localCount < 0) {
                r.message = "verifier: function \"" + fn.name +
                    "\" has negative localCount";
                return r;
            }
            for (const auto& block : fn.blocks) {
                bool terminated = false;
                for (const auto& instr : block.instructions) {
                    if (terminated) {
                        r.message = "verifier: instruction after terminator in block \"" +
                            block.label + "\" of function \"" + fn.name + "\"";
                        return r;
                    }
                    if (instr.opcode == VirOpcode::Const) {
                        if (instr.constIndex < 0 ||
                            instr.constIndex >= (int)module.constants.size()) {
                            r.message = "verifier: Const with invalid constIndex " +
                                std::to_string(instr.constIndex) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                        if (instr.destReg < 0 || instr.destReg >= fn.localCount) {
                            r.message = "verifier: Const destReg " +
                                std::to_string(instr.destReg) +
                                " out of localCount " +
                                std::to_string(fn.localCount) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                    }
                    if (instr.opcode == VirOpcode::Ret) {
                        if (instr.operandReg < 0 || instr.operandReg >= fn.localCount) {
                            r.message = "verifier: Ret operandReg " +
                                std::to_string(instr.operandReg) +
                                " out of localCount " +
                                std::to_string(fn.localCount) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                    }
                    if (isTerminator(instr.opcode)) terminated = true;
                }
                if (!terminated) {
                    r.message = "verifier: block \"" + block.label +
                        "\" of function \"" + fn.name +
                        "\" has no terminator instruction";
                    return r;
                }
            }
        }
        r.success = true;
        return r;
    }

    // ============================================================================
    // Lowering — Phase 2 handles only PassStmt / empty programs.
    // ============================================================================

    LoweringResult VirLowering::lower(const vayu::Block& program) const {
        LoweringResult result;

        VirModule& m = result.module;
        m.name = "main";

        VirFunction mainFn;
        mainFn.name = "main";
        mainFn.returnType = { VirTypeKind::None };

        VirBlock entry;
        entry.label = "entry";

        if (!lowerBlock(program, mainFn, entry, result)) {
            return result;
        }

        // Implicit return None.
        VirValue noneVal;
        noneVal.kind = VirValueKind::None;
        int noneIdx = (int)m.constants.size();
        m.constants.push_back(noneVal);

        int reg = mainFn.localCount++;
        VirInstruction constNone;
        constNone.opcode = VirOpcode::Const;
        constNone.destReg = reg;
        constNone.constIndex = noneIdx;
        entry.instructions.push_back(constNone);

        VirInstruction ret;
        ret.opcode = VirOpcode::Ret;
        ret.operandReg = reg;
        entry.instructions.push_back(ret);

        mainFn.blocks.push_back(std::move(entry));
        mainFn.entryBlock = 0;
        m.functions.push_back(std::move(mainFn));
        m.entryFunction = 0;

        result.success = true;
        return result;
    }

} // namespace vib