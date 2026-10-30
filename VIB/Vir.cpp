#include "Vir.hpp"
#include <sstream>
#include <unordered_map>

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
            case VirOpcode::Add:
                return "r" + std::to_string(i.destReg) + " = Add(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::Sub:
                return "r" + std::to_string(i.destReg) + " = Sub(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::Mul:
                return "r" + std::to_string(i.destReg) + " = Mul(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::Div:
                return "r" + std::to_string(i.destReg) + " = Div(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::Mod:
                return "r" + std::to_string(i.destReg) + " = Mod(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::Neg:
                return "r" + std::to_string(i.destReg) + " = Neg(r" +
                    std::to_string(i.operandReg) + ")";
            case VirOpcode::CmpEq:
                return "r" + std::to_string(i.destReg) + " = CmpEq(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::CmpNe:
                return "r" + std::to_string(i.destReg) + " = CmpNe(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::CmpLt:
                return "r" + std::to_string(i.destReg) + " = CmpLt(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::CmpLe:
                return "r" + std::to_string(i.destReg) + " = CmpLe(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::CmpGt:
                return "r" + std::to_string(i.destReg) + " = CmpGt(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::CmpGe:
                return "r" + std::to_string(i.destReg) + " = CmpGe(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::StoreLocal:
                return "local[" + std::to_string(i.localIndex) + "] = r" +
                    std::to_string(i.operandReg);
            case VirOpcode::LoadLocal:
                return "r" + std::to_string(i.destReg) + " = local[" +
                    std::to_string(i.localIndex) + "]";
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

        // ------------------------------------------------------------------------
// Phase 3 lowering context.
//
// Tracks per-function state needed to translate a supported AST subset
// into VIR:
//   - localsByName   : source name -> local slot index
//   - nextLocal      : next free local slot
//   - nextReg        : next free "register" (maps 1:1 to local slots for
//                      now; no register allocation yet)
//   - module         : the VirModule being built (needed for constants)
//   - fn             : the VirFunction being built
//   - entry          : the entry VirBlock being appended to
//   - result         : failure info
//
// Supported in Phase 3: integers, booleans, None, unary +/-, binary
// + - * / % == != < <= > >=, assignment of the form `name = expr` where
// `name` is a simple name. Everything else keeps its blocker.
// ------------------------------------------------------------------------
        struct LoweringContext {
            VirModule& module;
            VirFunction& fn;
            VirBlock& entry;
            LoweringResult& result;

            std::unordered_map<std::string, int> localsByName;
            int nextLocal = 0;
            int nextReg = 0;

            LoweringContext(VirModule& m, VirFunction& f, VirBlock& e, LoweringResult& r)
                : module(m), fn(f), entry(e), result(r) {
            }

            int allocReg() { return nextReg++; }

            void bumpLocalTo(int r) {
                if (r >= nextLocal) nextLocal = r + 1;
            }

            int addIntConst(long long value) {
                VirValue v;
                v.kind = VirValueKind::Int;
                v.intValue = value;
                int idx = (int)module.constants.size();
                module.constants.push_back(v);
                return idx;
            }

            int addBoolConst(bool value) {
                VirValue v;
                v.kind = VirValueKind::Bool;
                v.boolValue = value;
                int idx = (int)module.constants.size();
                module.constants.push_back(v);
                return idx;
            }

            int addNoneConst() {
                VirValue v;
                v.kind = VirValueKind::None;
                int idx = (int)module.constants.size();
                module.constants.push_back(v);
                return idx;
            }

            // Emits `dest = const [index]` and returns dest reg.
            int emitConst(int constIdx, vayu::SourceLocation loc) {
                int dest = allocReg();
                VirInstruction i;
                i.opcode = VirOpcode::Const;
                i.destReg = dest;
                i.constIndex = constIdx;
                i.sourceLoc = loc;
                entry.instructions.push_back(i);
                bumpLocalTo(dest);
                return dest;
            }

            // Returns true on success; on failure, fills result and returns false.
            bool lowerBlock(const vayu::Block& block);

            // Returns -1 on success, otherwise a blocker phase > 0.
            int lowerStmt(const vayu::Stmt* s);

            // Returns a reg id containing the value, or -1 on blocker (result set).
            int lowerExpr(const vayu::Expr* e);
        };

        bool LoweringContext::lowerBlock(const vayu::Block& block) {
            for (const auto& stmtPtr : block.stmts) {
                int blocker = lowerStmt(stmtPtr.get());
                if (blocker > 0) return false;
            }
            return true;
        }

        int LoweringContext::lowerStmt(const vayu::Stmt* s) {
            using SK = vayu::StmtKind;
            if (s->kind == SK::Pass) return 0;

            if (s->kind == SK::Block) {
                const auto* bs = static_cast<const vayu::BlockStmt*>(s);
                if (!lowerBlock(bs->body)) return result.plannedPhase;
                return 0;
            }

            if (s->kind == SK::Expr) {
                const auto* es = static_cast<const vayu::ExprStmt*>(s);
                int r = lowerExpr(es->expr.get());
                if (r < 0) return result.plannedPhase;
                return 0;
            }

            if (s->kind == SK::Assign) {
                const auto* as = static_cast<const vayu::AssignStmt*>(s);
                if (as->target->kind != vayu::ExprKind::NameRef) {
                    BlockerInfo b = blockerFor(s);
                    result.plannedPhase = b.phase;
                    result.failedFeatureName = b.featureName;
                    result.message = std::string("lowering for non-name assignment target "
                        "is not implemented yet (planned Phase ") +
                        std::to_string(b.phase) + ")";
                    return b.phase;
                }
                const auto* nr = static_cast<const vayu::NameRefExpr*>(as->target.get());
                int valueReg = lowerExpr(as->value.get());
                if (valueReg < 0) return result.plannedPhase;

                auto it = localsByName.find(nr->name);
                int localIdx;
                if (it == localsByName.end()) {
                    localIdx = nextLocal++;
                    localsByName[nr->name] = localIdx;
                }
                else {
                    localIdx = it->second;
                }
                VirInstruction store;
                store.opcode = VirOpcode::StoreLocal;
                store.operandReg = valueReg;
                store.localIndex = localIdx;
                store.sourceLoc = as->loc;
                entry.instructions.push_back(store);
                return 0;
            }

            BlockerInfo b = blockerFor(s);
            result.plannedPhase = b.phase;
            result.failedFeatureName = b.featureName;
            result.message = std::string("lowering for ") + b.stmtKindName +
                " is not implemented yet (planned Phase " +
                std::to_string(b.phase) + ")";
            return b.phase;
        }

        int LoweringContext::lowerExpr(const vayu::Expr* e) {
            using EK = vayu::ExprKind;
            switch (e->kind) {
            case EK::IntLit: {
                const auto* n = static_cast<const vayu::IntLitExpr*>(e);
                return emitConst(addIntConst(n->value), e->loc);
            }
            case EK::BoolLit: {
                const auto* n = static_cast<const vayu::BoolLitExpr*>(e);
                return emitConst(addBoolConst(n->value), e->loc);
            }
            case EK::NoneLit:
                return emitConst(addNoneConst(), e->loc);
            case EK::Grouping:
                return lowerExpr(static_cast<const vayu::GroupingExpr*>(e)->inner.get());
            case EK::NameRef: {
                const auto* n = static_cast<const vayu::NameRefExpr*>(e);
                auto it = localsByName.find(n->name);
                if (it == localsByName.end()) {
                    result.plannedPhase = 3;
                    result.failedFeatureName = "BasicArithmetic";
                    result.message = "lowering for name '" + n->name +
                        "' before assignment is not implemented yet (planned Phase 3)";
                    return -1;
                }
                int dest = allocReg();
                VirInstruction i;
                i.opcode = VirOpcode::LoadLocal;
                i.destReg = dest;
                i.localIndex = it->second;
                i.sourceLoc = e->loc;
                entry.instructions.push_back(i);
                bumpLocalTo(dest);
                return dest;
            }
            case EK::Unary: {
                const auto* n = static_cast<const vayu::UnaryExpr*>(e);
                if (n->op == vayu::UnOp::Pos) {
                    return lowerExpr(n->operand.get());
                }
                if (n->op != vayu::UnOp::Neg) {
                    result.plannedPhase = 3;
                    result.failedFeatureName = "BasicArithmetic";
                    result.message = std::string("lowering for unary operator '") +
                        vayu::unOpName(n->op) +
                        "' is not implemented yet (planned Phase 3)";
                    return -1;
                }
                int src = lowerExpr(n->operand.get());
                if (src < 0) return -1;
                int dest = allocReg();
                VirInstruction i;
                i.opcode = VirOpcode::Neg;
                i.destReg = dest;
                i.operandReg = src;
                i.sourceLoc = e->loc;
                entry.instructions.push_back(i);
                bumpLocalTo(dest);
                return dest;
            }
            case EK::Binary: {
                const auto* n = static_cast<const vayu::BinaryExpr*>(e);
                VirOpcode op;
                switch (n->op) {
                case vayu::BinOp::Add:    op = VirOpcode::Add;   break;
                case vayu::BinOp::Sub:    op = VirOpcode::Sub;   break;
                case vayu::BinOp::Mul:    op = VirOpcode::Mul;   break;
                case vayu::BinOp::Div:    op = VirOpcode::Div;   break;
                case vayu::BinOp::Mod:    op = VirOpcode::Mod;   break;
                case vayu::BinOp::Eq:     op = VirOpcode::CmpEq; break;
                case vayu::BinOp::NotEq:  op = VirOpcode::CmpNe; break;
                case vayu::BinOp::Lt:     op = VirOpcode::CmpLt; break;
                case vayu::BinOp::LtEq:   op = VirOpcode::CmpLe; break;
                case vayu::BinOp::Gt:     op = VirOpcode::CmpGt; break;
                case vayu::BinOp::GtEq:   op = VirOpcode::CmpGe; break;
                default:
                    result.plannedPhase = 3;
                    result.failedFeatureName = "BasicArithmetic";
                    result.message = std::string("lowering for binary operator '") +
                        vayu::binOpName(n->op) +
                        "' is not implemented yet (planned Phase 3)";
                    return -1;
                }
                int lhs = lowerExpr(n->lhs.get());
                if (lhs < 0) return -1;
                int rhs = lowerExpr(n->rhs.get());
                if (rhs < 0) return -1;
                int dest = allocReg();
                VirInstruction i;
                i.opcode = op;
                i.destReg = dest;
                i.lhsReg = lhs;
                i.rhsReg = rhs;
                i.sourceLoc = e->loc;
                entry.instructions.push_back(i);
                bumpLocalTo(dest);
                return dest;
            }
            default:
                result.plannedPhase = 3;
                result.failedFeatureName = "BasicArithmetic";
                result.message = "lowering for this expression kind is not implemented yet "
                    "(planned Phase 3)";
                return -1;
            }
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
                    auto regOK = [&](int reg) {
                        return reg >= 0 && reg < fn.localCount;
                        };
                    if (instr.opcode == VirOpcode::Const) {
                        if (instr.constIndex < 0 ||
                            instr.constIndex >= (int)module.constants.size()) {
                            r.message = "verifier: Const with invalid constIndex " +
                                std::to_string(instr.constIndex) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                        if (!regOK(instr.destReg)) {
                            r.message = "verifier: Const destReg " +
                                std::to_string(instr.destReg) +
                                " out of localCount " +
                                std::to_string(fn.localCount) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                    }
                    if (instr.opcode == VirOpcode::Ret) {
                        if (!regOK(instr.operandReg)) {
                            r.message = "verifier: Ret operandReg " +
                                std::to_string(instr.operandReg) +
                                " out of localCount " +
                                std::to_string(fn.localCount) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                    }
                    if (instr.opcode == VirOpcode::Add ||
                        instr.opcode == VirOpcode::Sub ||
                        instr.opcode == VirOpcode::Mul ||
                        instr.opcode == VirOpcode::Div ||
                        instr.opcode == VirOpcode::Mod ||
                        instr.opcode == VirOpcode::CmpEq ||
                        instr.opcode == VirOpcode::CmpNe ||
                        instr.opcode == VirOpcode::CmpLt ||
                        instr.opcode == VirOpcode::CmpLe ||
                        instr.opcode == VirOpcode::CmpGt ||
                        instr.opcode == VirOpcode::CmpGe) {
                        if (!regOK(instr.destReg) ||
                            !regOK(instr.lhsReg) ||
                            !regOK(instr.rhsReg)) {
                            r.message = "verifier: binary op with out-of-range reg in function \"" +
                                fn.name + "\"";
                            return r;
                        }
                    }
                    if (instr.opcode == VirOpcode::Neg) {
                        if (!regOK(instr.destReg) || !regOK(instr.operandReg)) {
                            r.message = "verifier: Neg with out-of-range reg in function \"" +
                                fn.name + "\"";
                            return r;
                        }
                    }
                    if (instr.opcode == VirOpcode::StoreLocal) {
                        if (!regOK(instr.operandReg)) {
                            r.message = "verifier: StoreLocal operandReg out of range in function \"" +
                                fn.name + "\"";
                            return r;
                        }
                        if (instr.localIndex < 0 || instr.localIndex >= fn.localCount) {
                            r.message = "verifier: StoreLocal localIndex " +
                                std::to_string(instr.localIndex) +
                                " out of range in function \"" + fn.name + "\"";
                            return r;
                        }
                    }
                    if (instr.opcode == VirOpcode::LoadLocal) {
                        if (!regOK(instr.destReg)) {
                            r.message = "verifier: LoadLocal destReg out of range in function \"" +
                                fn.name + "\"";
                            return r;
                        }
                        if (instr.localIndex < 0 || instr.localIndex >= fn.localCount) {
                            r.message = "verifier: LoadLocal localIndex " +
                                std::to_string(instr.localIndex) +
                                " out of range in function \"" + fn.name + "\"";
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

        LoweringContext ctx(m, mainFn, entry, result);
        if (!ctx.lowerBlock(program)) {
            return result;
        }

        // Implicit return None.
        int noneIdx = ctx.addNoneConst();
        int retReg = ctx.emitConst(noneIdx, vayu::SourceLocation{});

        VirInstruction ret;
        ret.opcode = VirOpcode::Ret;
        ret.operandReg = retReg;
        entry.instructions.push_back(ret);

        mainFn.localCount = ctx.nextLocal;
        mainFn.blocks.push_back(std::move(entry));
        mainFn.entryBlock = 0;
        m.functions.push_back(std::move(mainFn));
        m.entryFunction = 0;

        result.success = true;
        return result;
    }

} // namespace vib