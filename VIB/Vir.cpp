#include "Vir.hpp"
#include <sstream>
#include <string>
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

        const char* builtinName(VirBuiltin b) {
            switch (b) {
            case VirBuiltin::Print:         return "Print";
            case VirBuiltin::ToStr:         return "ToStr";
            case VirBuiltin::ToInt:         return "ToInt";
            case VirBuiltin::ToFloat:       return "ToFloat";
            case VirBuiltin::ToBool:        return "ToBool";
            case VirBuiltin::LenStr:        return "LenStr";
            case VirBuiltin::StrUpper:      return "StrUpper";
            case VirBuiltin::StrLower:      return "StrLower";
            case VirBuiltin::StrStrip:      return "StrStrip";
            case VirBuiltin::StrContains:   return "StrContains";
            case VirBuiltin::StrFind:       return "StrFind";
            case VirBuiltin::StrStartsWith: return "StrStartsWith";
            case VirBuiltin::StrEndsWith:   return "StrEndsWith";
            case VirBuiltin::StrIsDigit:    return "StrIsDigit";
            case VirBuiltin::StrIsAlpha:    return "StrIsAlpha";
            case VirBuiltin::StrIsSpace:    return "StrIsSpace";
            case VirBuiltin::StrReplace:    return "StrReplace";
            case VirBuiltin::StrCharAt:     return "StrCharAt";
            case VirBuiltin::StrToInt:      return "StrToInt";
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
            case VirOpcode::Pow:
                return "r" + std::to_string(i.destReg) + " = Pow(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::Neg:
                return "r" + std::to_string(i.destReg) + " = Neg(r" +
                    std::to_string(i.operandReg) + ")";
            case VirOpcode::LogAnd:
                return "r" + std::to_string(i.destReg) + " = LogAnd(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::LogOr:
                return "r" + std::to_string(i.destReg) + " = LogOr(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::LogNot:
                return "r" + std::to_string(i.destReg) + " = LogNot(r" +
                    std::to_string(i.operandReg) + ")";
            case VirOpcode::StrRepeat:
                return "r" + std::to_string(i.destReg) + " = StrRepeat(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
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
            case VirOpcode::StrConcat:
                return "r" + std::to_string(i.destReg) + " = StrConcat(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::StrEq:
                return "r" + std::to_string(i.destReg) + " = StrEq(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::StrNe:
                return "r" + std::to_string(i.destReg) + " = StrNe(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::StrCmpLt:
                return "r" + std::to_string(i.destReg) + " = StrCmpLt(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::Call: {
                std::string s = "r" + std::to_string(i.destReg) +
                    " = Call(fn[" + std::to_string(i.funcIndex) + "]";
                for (int r : i.argRegs) {
                    s += ", r" + std::to_string(r);
                }
                s += ")";
                return s;
            }
            case VirOpcode::CallBuiltin: {
                std::string s = "r" + std::to_string(i.destReg) +
                    " = CallBuiltin(" + builtinName(i.builtin);
                for (int r : i.argRegs) {
                    s += ", r" + std::to_string(r);
                }
                s += ")";
                return s;
            }
            case VirOpcode::Jump:
                return "Jump(@" + std::to_string(i.targetBlock) + ")";
            case VirOpcode::JumpIfFalse:
                return "JumpIfFalse(r" + std::to_string(i.operandReg) +
                    ", @" + std::to_string(i.targetBlock) + ")";
            case VirOpcode::ListNew:
                return "r" + std::to_string(i.destReg) + " = ListNew";
            case VirOpcode::ListPush:
                return "ListPush(r" + std::to_string(i.lhsReg) + ", r" +
                    std::to_string(i.operandReg) + ")";
            case VirOpcode::ListGet:
                return "r" + std::to_string(i.destReg) + " = ListGet(r" +
                    std::to_string(i.lhsReg) + ", r" + std::to_string(i.rhsReg) + ")";
            case VirOpcode::ListSet:
                return "ListSet(r" + std::to_string(i.lhsReg) + ", r" +
                    std::to_string(i.rhsReg) + ", r" + std::to_string(i.operandReg) + ")";
            case VirOpcode::ListLen:
                return "r" + std::to_string(i.destReg) + " = ListLen(r" +
                    std::to_string(i.operandReg) + ")";
            }
            return "?";
        }

        bool isTerminator(VirOpcode op) {
            return op == VirOpcode::Ret || op == VirOpcode::Jump;
        }

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
        // Phase 3/4/5 lowering context.
        // ------------------------------------------------------------------------
        struct LoweringContext {
            VirModule& module;
            VirFunction& fn;
            VirBlock& entry;
            LoweringResult& result;

            std::unordered_map<std::string, int> localsByName;
            int nextLocal = 0;
            int nextReg = 0;

            std::unordered_map<std::string, VirTypeKind> varTypes;
            std::unordered_map<std::string, int> topLevelFunctions;

            // Phase 6.1: break/continue patch sites (instruction indices).
            struct LoopFrame {
                std::vector<int> breakJumps;    // Jump instr indices to patch to exit
                std::vector<int> continueJumps; // Jump instr indices to patch to header
                int continueTarget = -1;        // set after header is known
            };
            std::vector<LoopFrame> loopStack;

            LoweringContext(VirModule& m, VirFunction& f, VirBlock& e, LoweringResult& r)
                : module(m), fn(f), entry(e), result(r) {
            }

            int allocReg() { return nextReg++; }
            void bumpLocalTo(int r) { if (r >= nextLocal) nextLocal = r + 1; }

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
            int addStrConst(const std::string& value) {
                VirValue v;
                v.kind = VirValueKind::Str;
                v.strValue = value;
                int idx = (int)module.constants.size();
                module.constants.push_back(v);
                return idx;
            }

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

            bool lowerBlock(const vayu::Block& block);
            int  lowerStmt(const vayu::Stmt* s);
            int  lowerExpr(const vayu::Expr* e);
            int  lowerCall(const vayu::CallExpr* c);
            VirTypeKind inferType(const vayu::Expr* e) const;
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

            if (s->kind == SK::Return) {
                const auto* rs = static_cast<const vayu::ReturnStmt*>(s);
                int reg;
                if (rs->value) {
                    reg = lowerExpr(rs->value.get());
                    if (reg < 0) return result.plannedPhase;
                }
                else {
                    int noneIdx = addNoneConst();
                    reg = emitConst(noneIdx, rs->loc);
                }
                VirInstruction ret;
                ret.opcode = VirOpcode::Ret;
                ret.operandReg = reg;
                ret.sourceLoc = rs->loc;
                entry.instructions.push_back(ret);
                return 0;
            }

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

            if (s->kind == SK::AnnotAssign) {
                const auto* aa = static_cast<const vayu::AnnotAssignStmt*>(s);
                int valueReg;
                if (aa->value) {
                    valueReg = lowerExpr(aa->value.get());
                    if (valueReg < 0) return result.plannedPhase;
                }
                else {
                    int noneIdx = addNoneConst();
                    valueReg = emitConst(noneIdx, aa->loc);
                }
                int localIdx;
                auto it = localsByName.find(aa->name);
                if (it == localsByName.end()) {
                    localIdx = nextLocal++;
                    localsByName[aa->name] = localIdx;
                }
                else {
                    localIdx = it->second;
                }
                VirInstruction store;
                store.opcode = VirOpcode::StoreLocal;
                store.operandReg = valueReg;
                store.localIndex = localIdx;
                store.sourceLoc = aa->loc;
                entry.instructions.push_back(store);
                VirTypeKind tk = VirTypeKind::Any;
                if (aa->type && aa->type->kind == vayu::ExprKind::NameRef) {
                    const std::string& tn =
                        static_cast<const vayu::NameRefExpr*>(aa->type.get())->name;
                    if (tn == "int")        tk = VirTypeKind::Int;
                    else if (tn == "float") tk = VirTypeKind::Float;
                    else if (tn == "bool")  tk = VirTypeKind::Bool;
                    else if (tn == "str")   tk = VirTypeKind::Str;
                }
                varTypes[aa->name] = tk;
                return 0;
            }

            if (s->kind == SK::Assign) {
                const auto* as = static_cast<const vayu::AssignStmt*>(s);
                if (as->target->kind == vayu::ExprKind::Index) {
                    const auto* ix = static_cast<const vayu::IndexExpr*>(as->target.get());
                    int base = lowerExpr(ix->target.get());
                    if (base < 0) return result.plannedPhase;
                    int idx = lowerExpr(ix->index.get());
                    if (idx < 0) return result.plannedPhase;
                    int valueReg = lowerExpr(as->value.get());
                    if (valueReg < 0) return result.plannedPhase;
                    VirInstruction st;
                    st.opcode = VirOpcode::ListSet;
                    st.lhsReg = base;
                    st.rhsReg = idx;
                    st.operandReg = valueReg;
                    st.sourceLoc = as->loc;
                    entry.instructions.push_back(st);
                    return 0;
                }
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
                varTypes[nr->name] = inferType(as->value.get());
                return 0;
            }

            // Phase 6.1: If / Elif / Else.
            if (s->kind == SK::If) {
                const auto* is = static_cast<const vayu::IfStmt*>(s);
                std::vector<int> joinJumps;

                auto emitBranch = [&](const vayu::Expr* cond, const vayu::Block& body,
                    bool hasMore) -> int {
                    int condReg = lowerExpr(cond);
                    if (condReg < 0) return -1;
                    VirInstruction jfalse;
                    jfalse.opcode = VirOpcode::JumpIfFalse;
                    jfalse.operandReg = condReg;
                    jfalse.targetBlock = -1;
                    jfalse.sourceLoc = is->loc;
                    int jfalseIdx = (int)entry.instructions.size();
                    entry.instructions.push_back(jfalse);
                    if (!lowerBlock(body)) return -1;
                    if (hasMore) {
                        VirInstruction jjoin;
                        jjoin.opcode = VirOpcode::Jump;
                        jjoin.targetBlock = -1;
                        jjoin.sourceLoc = is->loc;
                        joinJumps.push_back((int)entry.instructions.size());
                        entry.instructions.push_back(jjoin);
                    }
                    entry.instructions[(size_t)jfalseIdx].targetBlock =
                        (int)entry.instructions.size();
                    return 0;
                };

                bool hasElifOrElse = !is->elifs.empty() || is->elseBody.has_value();
                if (emitBranch(is->cond.get(), is->thenBody, hasElifOrElse) < 0)
                    return result.plannedPhase;

                for (size_t ei = 0; ei < is->elifs.size(); ++ei) {
                    bool more = (ei + 1 < is->elifs.size()) || is->elseBody.has_value();
                    if (emitBranch(is->elifs[ei].cond.get(), is->elifs[ei].body, more) < 0)
                        return result.plannedPhase;
                }

                if (is->elseBody) {
                    if (!lowerBlock(*is->elseBody)) return result.plannedPhase;
                }

                int joinPc = (int)entry.instructions.size();
                for (int idx : joinJumps)
                    entry.instructions[(size_t)idx].targetBlock = joinPc;
                return 0;
            }

            // Phase 6.1: while
            if (s->kind == SK::While) {
                const auto* ws = static_cast<const vayu::WhileStmt*>(s);
                int header = (int)entry.instructions.size();

                loopStack.push_back(LoopFrame{});
                loopStack.back().continueTarget = header;

                int condReg = lowerExpr(ws->cond.get());
                if (condReg < 0) { loopStack.pop_back(); return result.plannedPhase; }

                VirInstruction jfalse;
                jfalse.opcode = VirOpcode::JumpIfFalse;
                jfalse.operandReg = condReg;
                jfalse.targetBlock = -1;
                jfalse.sourceLoc = ws->loc;
                int jfalseIdx = (int)entry.instructions.size();
                entry.instructions.push_back(jfalse);

                if (!lowerBlock(ws->body)) { loopStack.pop_back(); return result.plannedPhase; }

                VirInstruction jback;
                jback.opcode = VirOpcode::Jump;
                jback.targetBlock = header;
                jback.sourceLoc = ws->loc;
                entry.instructions.push_back(jback);

                int exitPc = (int)entry.instructions.size();
                entry.instructions[(size_t)jfalseIdx].targetBlock = exitPc;

                LoopFrame frame = std::move(loopStack.back());
                loopStack.pop_back();
                for (int idx : frame.breakJumps)
                    entry.instructions[(size_t)idx].targetBlock = exitPc;
                for (int idx : frame.continueJumps)
                    entry.instructions[(size_t)idx].targetBlock = header;
                return 0;
            }

            // Phase 6.1: break / continue
            if (s->kind == SK::Break) {
                if (loopStack.empty()) {
                    result.plannedPhase = 6;
                    result.failedFeatureName = "ControlFlow";
                    result.message = "break outside loop";
                    return 6;
                }
                VirInstruction j;
                j.opcode = VirOpcode::Jump;
                j.targetBlock = -1;
                j.sourceLoc = s->loc;
                loopStack.back().breakJumps.push_back((int)entry.instructions.size());
                entry.instructions.push_back(j);
                return 0;
            }
            if (s->kind == SK::Continue) {
                if (loopStack.empty()) {
                    result.plannedPhase = 6;
                    result.failedFeatureName = "ControlFlow";
                    result.message = "continue outside loop";
                    return 6;
                }
                VirInstruction j;
                j.opcode = VirOpcode::Jump;
                j.targetBlock = loopStack.back().continueTarget;
                j.sourceLoc = s->loc;
                if (j.targetBlock < 0) {
                    loopStack.back().continueJumps.push_back((int)entry.instructions.size());
                }
                entry.instructions.push_back(j);
                return 0;
            }

            // Phase 6.1: for x in range(N)  — only the range(N) form.
            if (s->kind == SK::For) {
                const auto* fs = static_cast<const vayu::ForStmt*>(s);
                // Require: iterable is Call range(single_arg)
                if (!fs->iterable || fs->iterable->kind != vayu::ExprKind::Call) {
                    result.plannedPhase = 6;
                    result.failedFeatureName = "ControlFlow";
                    result.message = "for: only 'for x in range(N)' is supported in Phase 6.1";
                    return 6;
                }
                const auto* call = static_cast<const vayu::CallExpr*>(fs->iterable.get());
                if (!call->callee || call->callee->kind != vayu::ExprKind::NameRef) {
                    result.plannedPhase = 6;
                    result.failedFeatureName = "ControlFlow";
                    result.message = "for: only 'for x in range(N)' is supported in Phase 6.1";
                    return 6;
                }
                const auto* calleen = static_cast<const vayu::NameRefExpr*>(call->callee.get());
                if (calleen->name != "range" || call->args.size() != 1) {
                    result.plannedPhase = 6;
                    result.failedFeatureName = "ControlFlow";
                    result.message = "for: only 'for x in range(N)' is supported in Phase 6.1";
                    return 6;
                }

                int limitReg = lowerExpr(call->args[0].value.get());
                if (limitReg < 0) return result.plannedPhase;

                // i = 0
                int zeroIdx = addIntConst(0);
                int zeroReg = emitConst(zeroIdx, fs->loc);
                int localIdx;
                auto it = localsByName.find(fs->targetName);
                if (it == localsByName.end()) {
                    localIdx = nextLocal++;
                    localsByName[fs->targetName] = localIdx;
                    if (localIdx >= nextReg) nextReg = localIdx + 1;
                } else {
                    localIdx = it->second;
                }
                {
                    VirInstruction store;
                    store.opcode = VirOpcode::StoreLocal;
                    store.operandReg = zeroReg;
                    store.localIndex = localIdx;
                    store.sourceLoc = fs->loc;
                    entry.instructions.push_back(store);
                }
                varTypes[fs->targetName] = VirTypeKind::Int;

                int header = (int)entry.instructions.size();
                loopStack.push_back(LoopFrame{});
                loopStack.back().continueTarget = header;

                // cond: i < limit
                int iLoad = allocReg();
                {
                    VirInstruction ld;
                    ld.opcode = VirOpcode::LoadLocal;
                    ld.destReg = iLoad;
                    ld.localIndex = localIdx;
                    ld.sourceLoc = fs->loc;
                    entry.instructions.push_back(ld);
                    bumpLocalTo(iLoad);
                }
                int cmpReg = allocReg();
                {
                    VirInstruction cmp;
                    cmp.opcode = VirOpcode::CmpLt;
                    cmp.destReg = cmpReg;
                    cmp.lhsReg = iLoad;
                    cmp.rhsReg = limitReg;
                    cmp.sourceLoc = fs->loc;
                    entry.instructions.push_back(cmp);
                    bumpLocalTo(cmpReg);
                }
                VirInstruction jfalse;
                jfalse.opcode = VirOpcode::JumpIfFalse;
                jfalse.operandReg = cmpReg;
                jfalse.targetBlock = -1;
                jfalse.sourceLoc = fs->loc;
                int jfalseIdx = (int)entry.instructions.size();
                entry.instructions.push_back(jfalse);

                if (!lowerBlock(fs->body)) { loopStack.pop_back(); return result.plannedPhase; }

                // i = i + 1  (continue target = here for for-loops)
                int contPc = (int)entry.instructions.size();
                loopStack.back().continueTarget = contPc;
                {
                    int oneIdx = addIntConst(1);
                    int oneReg = emitConst(oneIdx, fs->loc);
                    int i2 = allocReg();
                    VirInstruction ld;
                    ld.opcode = VirOpcode::LoadLocal;
                    ld.destReg = i2;
                    ld.localIndex = localIdx;
                    ld.sourceLoc = fs->loc;
                    entry.instructions.push_back(ld);
                    bumpLocalTo(i2);
                    int sum = allocReg();
                    VirInstruction add;
                    add.opcode = VirOpcode::Add;
                    add.destReg = sum;
                    add.lhsReg = i2;
                    add.rhsReg = oneReg;
                    add.sourceLoc = fs->loc;
                    entry.instructions.push_back(add);
                    bumpLocalTo(sum);
                    VirInstruction st;
                    st.opcode = VirOpcode::StoreLocal;
                    st.operandReg = sum;
                    st.localIndex = localIdx;
                    st.sourceLoc = fs->loc;
                    entry.instructions.push_back(st);
                }

                VirInstruction jback;
                jback.opcode = VirOpcode::Jump;
                jback.targetBlock = header;
                jback.sourceLoc = fs->loc;
                entry.instructions.push_back(jback);

                int exitPc = (int)entry.instructions.size();
                entry.instructions[(size_t)jfalseIdx].targetBlock = exitPc;

                LoopFrame frame = std::move(loopStack.back());
                loopStack.pop_back();
                for (int idx : frame.breakJumps)
                    entry.instructions[(size_t)idx].targetBlock = exitPc;
                for (int idx : frame.continueJumps)
                    entry.instructions[(size_t)idx].targetBlock = contPc;
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
            case EK::StringLit: {
                const auto* n = static_cast<const vayu::StringLitExpr*>(e);
                return emitConst(addStrConst(n->value), e->loc);
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
                if (n->op == vayu::UnOp::Not) {
                    int src = lowerExpr(n->operand.get());
                    if (src < 0) return -1;
                    int dest = allocReg();
                    VirInstruction i;
                    i.opcode = VirOpcode::LogNot;
                    i.destReg = dest;
                    i.operandReg = src;
                    i.sourceLoc = e->loc;
                    entry.instructions.push_back(i);
                    bumpLocalTo(dest);
                    return dest;
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
                bool needsStringOp = false;
                switch (n->op) {
                case vayu::BinOp::Add:      op = VirOpcode::Add; break;
                case vayu::BinOp::Sub:      op = VirOpcode::Sub; break;
                case vayu::BinOp::Mul:      op = VirOpcode::Mul; break;
                case vayu::BinOp::Div:      op = VirOpcode::Div; break;
                case vayu::BinOp::FloorDiv: op = VirOpcode::Div; break; // int floor div via same opcode
                case vayu::BinOp::Mod:      op = VirOpcode::Mod; break;
                case vayu::BinOp::Pow:      op = VirOpcode::Pow; break;
                case vayu::BinOp::And:      op = VirOpcode::LogAnd; break;
                case vayu::BinOp::Or:       op = VirOpcode::LogOr; break;
                case vayu::BinOp::Eq:     op = VirOpcode::CmpEq; needsStringOp = true; break;
                case vayu::BinOp::NotEq:  op = VirOpcode::CmpNe; needsStringOp = true; break;
                case vayu::BinOp::Lt:     op = VirOpcode::CmpLt; needsStringOp = true; break;
                case vayu::BinOp::LtEq:   op = VirOpcode::CmpLe; needsStringOp = true; break;
                case vayu::BinOp::Gt:     op = VirOpcode::CmpGt; needsStringOp = true; break;
                case vayu::BinOp::GtEq:   op = VirOpcode::CmpGe; needsStringOp = true; break;
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

                bool lhsIsStr = (n->lhs->kind == vayu::ExprKind::StringLit);
                bool rhsIsStr = (n->rhs->kind == vayu::ExprKind::StringLit);
                VirTypeKind lt = inferType(n->lhs.get());
                VirTypeKind rt = inferType(n->rhs.get());
                if (lhsIsStr || rhsIsStr || lt == VirTypeKind::Str || rt == VirTypeKind::Str) {
                    if (n->op == vayu::BinOp::Add) {
                        op = VirOpcode::StrConcat;
                    }
                    else if (n->op == vayu::BinOp::Mul) {
                        op = VirOpcode::StrRepeat;
                    }
                    else if (n->op == vayu::BinOp::Eq) {
                        op = VirOpcode::StrEq;
                    }
                    else if (n->op == vayu::BinOp::NotEq) {
                        op = VirOpcode::StrNe;
                    }
                    else if (n->op == vayu::BinOp::Lt) {
                        op = VirOpcode::StrCmpLt;
                    }
                    else if (needsStringOp) {
                        result.plannedPhase = 4;
                        result.failedFeatureName = "Strings";
                        result.message = std::string("lowering for string operator '") +
                            vayu::binOpName(n->op) +
                            "' is not implemented yet (planned Phase 4)";
                        return -1;
                    }
                }

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
            case EK::ListLit: {
                const auto* n = static_cast<const vayu::ListLitExpr*>(e);
                int dest = allocReg();
                VirInstruction mk;
                mk.opcode = VirOpcode::ListNew;
                mk.destReg = dest;
                mk.sourceLoc = e->loc;
                entry.instructions.push_back(mk);
                bumpLocalTo(dest);
                for (const auto& el : n->elements) {
                    int er = lowerExpr(el.get());
                    if (er < 0) return -1;
                    VirInstruction push;
                    push.opcode = VirOpcode::ListPush;
                    push.lhsReg = dest;
                    push.operandReg = er;
                    push.sourceLoc = e->loc;
                    entry.instructions.push_back(push);
                }
                return dest;
            }
            case EK::Index: {
                const auto* n = static_cast<const vayu::IndexExpr*>(e);
                int base = lowerExpr(n->target.get());
                if (base < 0) return -1;
                int idx = lowerExpr(n->index.get());
                if (idx < 0) return -1;
                int dest = allocReg();
                VirInstruction g;
                g.opcode = VirOpcode::ListGet;
                g.destReg = dest;
                g.lhsReg = base;
                g.rhsReg = idx;
                g.sourceLoc = e->loc;
                entry.instructions.push_back(g);
                bumpLocalTo(dest);
                return dest;
            }
            case EK::Call:
                return lowerCall(static_cast<const vayu::CallExpr*>(e));
            default:
                result.plannedPhase = 3;
                result.failedFeatureName = "BasicArithmetic";
                result.message = "lowering for this expression kind is not implemented yet "
                    "(planned Phase 3)";
                return -1;
            }
        }

        VirTypeKind LoweringContext::inferType(const vayu::Expr* e) const {
            using EK = vayu::ExprKind;
            if (!e) return VirTypeKind::None;
            switch (e->kind) {
            case EK::IntLit:    return VirTypeKind::Int;
            case EK::FloatLit:  return VirTypeKind::Float;
            case EK::BoolLit:   return VirTypeKind::Bool;
            case EK::StringLit: return VirTypeKind::Str;
            case EK::NoneLit:   return VirTypeKind::None;
            case EK::NameRef: {
                const auto* n = static_cast<const vayu::NameRefExpr*>(e);
                auto it = varTypes.find(n->name);
                return it == varTypes.end() ? VirTypeKind::Any : it->second;
            }
            case EK::Grouping:
                return inferType(static_cast<const vayu::GroupingExpr*>(e)->inner.get());
            case EK::Binary: {
                const auto* n = static_cast<const vayu::BinaryExpr*>(e);
                VirTypeKind l = inferType(n->lhs.get());
                VirTypeKind r = inferType(n->rhs.get());
                if (n->op == vayu::BinOp::Add) {
                    if (l == VirTypeKind::Str || r == VirTypeKind::Str) return VirTypeKind::Str;
                    if (l == VirTypeKind::Int && r == VirTypeKind::Int) return VirTypeKind::Int;
                    if (l == VirTypeKind::Float || r == VirTypeKind::Float) return VirTypeKind::Float;
                    return VirTypeKind::Any;
                }
                if (n->op == vayu::BinOp::Eq || n->op == vayu::BinOp::NotEq ||
                    n->op == vayu::BinOp::Lt || n->op == vayu::BinOp::LtEq ||
                    n->op == vayu::BinOp::Gt || n->op == vayu::BinOp::GtEq) {
                    return VirTypeKind::Bool;
                }
                if (l == VirTypeKind::Int && r == VirTypeKind::Int) return VirTypeKind::Int;
                if ((l == VirTypeKind::Int || l == VirTypeKind::Float) &&
                    (r == VirTypeKind::Int || r == VirTypeKind::Float)) return VirTypeKind::Float;
                return VirTypeKind::Any;
            }
            case EK::Call: {
                const auto* c = static_cast<const vayu::CallExpr*>(e);
                if (c->callee->kind == EK::NameRef) {
                    const auto* n = static_cast<const vayu::NameRefExpr*>(c->callee.get());
                    if (n->name == "str")   return VirTypeKind::Str;
                    if (n->name == "int")   return VirTypeKind::Int;
                    if (n->name == "float") return VirTypeKind::Float;
                    if (n->name == "bool")  return VirTypeKind::Bool;
                    if (n->name == "len")   return VirTypeKind::Int;
                    return VirTypeKind::Any;
                }
                return VirTypeKind::Any;
            }
            default:
                return VirTypeKind::Any;
            }
        }

        int LoweringContext::lowerCall(const vayu::CallExpr* c) {
            using EK = vayu::ExprKind;

            if (c->callee->kind == EK::Attr) {
                const auto* attr = static_cast<const vayu::AttrExpr*>(c->callee.get());
                if (attr->target->kind != EK::NameRef) {
                    result.plannedPhase = 10;
                    result.failedFeatureName = "Classes";
                    result.message = "lowering for method call on non-name receiver is not "
                        "implemented yet (planned Phase 10)";
                    return -1;
                }
                const auto* recvName =
                    static_cast<const vayu::NameRefExpr*>(attr->target.get());
                VirTypeKind recvType = VirTypeKind::Any;
                auto tit = varTypes.find(recvName->name);
                if (tit != varTypes.end()) recvType = tit->second;

                if (recvType != VirTypeKind::Str) {
                    result.plannedPhase = 10;
                    result.failedFeatureName = "Classes";
                    result.message = "lowering for method call on non-str variable \"" +
                        recvName->name + "\" is not implemented yet (planned Phase 10)";
                    return -1;
                }

                int recvReg = lowerExpr(attr->target.get());
                if (recvReg < 0) return -1;

                std::vector<int> argRegs;
                argRegs.push_back(recvReg);
                for (const auto& a : c->args) {
                    int r = lowerExpr(a.value.get());
                    if (r < 0) return -1;
                    argRegs.push_back(r);
                }

                VirBuiltin builtin;
                const std::string& m = attr->name;
                if (m == "upper")       builtin = VirBuiltin::StrUpper;
                else if (m == "lower")       builtin = VirBuiltin::StrLower;
                else if (m == "strip")       builtin = VirBuiltin::StrStrip;
                else if (m == "contains")    builtin = VirBuiltin::StrContains;
                else if (m == "find")        builtin = VirBuiltin::StrFind;
                else if (m == "starts_with") builtin = VirBuiltin::StrStartsWith;
                else if (m == "ends_with")   builtin = VirBuiltin::StrEndsWith;
                else if (m == "is_digit")    builtin = VirBuiltin::StrIsDigit;
                else if (m == "is_alpha")    builtin = VirBuiltin::StrIsAlpha;
                else if (m == "is_space")    builtin = VirBuiltin::StrIsSpace;
                else if (m == "replace")     builtin = VirBuiltin::StrReplace;
                else if (m == "char_at")     builtin = VirBuiltin::StrCharAt;
                else if (m == "to_int")      builtin = VirBuiltin::StrToInt;
                else {
                    result.plannedPhase = 4;
                    result.failedFeatureName = "Strings";
                    result.message = "lowering for string method '." + m +
                        "' is not implemented yet (planned Phase 4)";
                    return -1;
                }

                int dest = allocReg();
                VirInstruction i;
                i.opcode = VirOpcode::CallBuiltin;
                i.destReg = dest;
                i.builtin = builtin;
                i.argRegs = std::move(argRegs);
                i.sourceLoc = c->loc;
                entry.instructions.push_back(i);
                bumpLocalTo(dest);
                return dest;
            }

            if (c->callee->kind != EK::NameRef) {
                result.plannedPhase = 10;
                result.failedFeatureName = "Classes";
                result.message = "lowering for call with non-name callee is not implemented "
                    "yet (planned Phase 10)";
                return -1;
            }
            const auto* callee = static_cast<const vayu::NameRefExpr*>(c->callee.get());
            const std::string& name = callee->name;

            VirBuiltin builtinOp = VirBuiltin::Print;
            bool isBuiltin = true;
            if (name == "print") builtinOp = VirBuiltin::Print;
            else if (name == "str")   builtinOp = VirBuiltin::ToStr;
            else if (name == "int")   builtinOp = VirBuiltin::ToInt;
            else if (name == "float") builtinOp = VirBuiltin::ToFloat;
            else if (name == "bool")  builtinOp = VirBuiltin::ToBool;
            else if (name == "len")   builtinOp = VirBuiltin::LenStr;
            else                      isBuiltin = false;

            if (isBuiltin) {
                if (name == "len") {
                    if (c->args.size() != 1) {
                        result.plannedPhase = 5;
                        result.failedFeatureName = "Functions";
                        result.message = "len() requires exactly 1 argument";
                        return -1;
                    }
                    // Phase 7.1: len works on str and list (resolved at runtime).
                }

                std::vector<int> argRegs;
                for (const auto& a : c->args) {
                    int r = lowerExpr(a.value.get());
                    if (r < 0) return -1;
                    argRegs.push_back(r);
                }
                int dest = allocReg();
                VirInstruction i;
                i.opcode = VirOpcode::CallBuiltin;
                i.destReg = dest;
                i.builtin = builtinOp;
                i.argRegs = std::move(argRegs);
                i.sourceLoc = c->loc;
                entry.instructions.push_back(i);
                bumpLocalTo(dest);
                return dest;
            }

            auto it = topLevelFunctions.find(name);
            if (it == topLevelFunctions.end()) {
                result.plannedPhase = 10;
                result.failedFeatureName = "Functions";
                result.message = "call to unknown function \"" + name +
                    "\" (higher-order function calls are Phase 10)";
                return -1;
            }
            int funcIndex = it->second;
            std::vector<int> argRegs;
            for (const auto& a : c->args) {
                int r = lowerExpr(a.value.get());
                if (r < 0) return -1;
                argRegs.push_back(r);
            }
            int dest = allocReg();
            VirInstruction i;
            i.opcode = VirOpcode::Call;
            i.destReg = dest;
            i.funcIndex = funcIndex;
            i.argRegs = std::move(argRegs);
            i.sourceLoc = c->loc;
            entry.instructions.push_back(i);
            bumpLocalTo(dest);
            return dest;
        }

    } // namespace

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
                    // Phase 6.2: branch-target verification (instruction indices).
                    if (instr.opcode == VirOpcode::Jump ||
                        instr.opcode == VirOpcode::JumpIfFalse) {
                        if (instr.targetBlock < 0 ||
                            instr.targetBlock > (int)block.instructions.size()) {
                            r.message = "verifier: jump target " +
                                std::to_string(instr.targetBlock) +
                                " out of range in function \"" + fn.name + "\"";
                            return r;
                        }
                        if (instr.opcode == VirOpcode::JumpIfFalse &&
                            !regOK(instr.operandReg)) {
                            r.message = "verifier: JumpIfFalse operandReg out of range in function \"" +
                                fn.name + "\"";
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
                        instr.opcode == VirOpcode::CmpGe ||
                        instr.opcode == VirOpcode::StrConcat ||
                        instr.opcode == VirOpcode::StrEq ||
                        instr.opcode == VirOpcode::StrNe ||
                        instr.opcode == VirOpcode::StrCmpLt) {
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
                    if (instr.opcode == VirOpcode::Call) {
                        if (instr.funcIndex < 0 ||
                            instr.funcIndex >= (int)module.functions.size()) {
                            r.message = "verifier: Call funcIndex " +
                                std::to_string(instr.funcIndex) +
                                " out of range in function \"" + fn.name + "\"";
                            return r;
                        }
                        if (!regOK(instr.destReg)) {
                            r.message = "verifier: Call destReg out of range in function \"" +
                                fn.name + "\"";
                            return r;
                        }
                        const auto& callee = module.functions[(size_t)instr.funcIndex];
                        if (instr.argRegs.size() != callee.paramNames.size()) {
                            r.message = "verifier: Call to \"" + callee.name +
                                "\" passes " + std::to_string(instr.argRegs.size()) +
                                " arg(s), callee expects " +
                                std::to_string(callee.paramNames.size()) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                        for (int a : instr.argRegs) {
                            if (!regOK(a)) {
                                r.message = "verifier: Call argReg out of range in function \"" +
                                    fn.name + "\"";
                                return r;
                            }
                        }
                    }
                    if (instr.opcode == VirOpcode::CallBuiltin) {
                        if (!regOK(instr.destReg)) {
                            r.message = "verifier: CallBuiltin destReg out of range in function \"" +
                                fn.name + "\"";
                            return r;
                        }
                        size_t n = instr.argRegs.size();
                        bool arityOK = true;
                        switch (instr.builtin) {
                        case VirBuiltin::Print:         arityOK = true; break;
                        case VirBuiltin::ToStr:
                        case VirBuiltin::ToInt:
                        case VirBuiltin::ToFloat:
                        case VirBuiltin::ToBool:
                        case VirBuiltin::LenStr:
                        case VirBuiltin::StrUpper:
                        case VirBuiltin::StrLower:
                        case VirBuiltin::StrStrip:
                        case VirBuiltin::StrIsDigit:
                        case VirBuiltin::StrIsAlpha:
                        case VirBuiltin::StrIsSpace:
                        case VirBuiltin::StrToInt:
                            arityOK = (n == 1); break;
                        case VirBuiltin::StrContains:
                        case VirBuiltin::StrFind:
                        case VirBuiltin::StrStartsWith:
                        case VirBuiltin::StrEndsWith:
                        case VirBuiltin::StrCharAt:
                            arityOK = (n == 2); break;
                        case VirBuiltin::StrReplace:
                            arityOK = (n == 3); break;
                        }
                        if (!arityOK) {
                            r.message = "verifier: CallBuiltin(" +
                                std::string(builtinName(instr.builtin)) +
                                ") wrong arity " + std::to_string(n) +
                                " in function \"" + fn.name + "\"";
                            return r;
                        }
                        for (int a : instr.argRegs) {
                            if (!regOK(a)) {
                                r.message = "verifier: CallBuiltin argReg out of range in function \"" +
                                    fn.name + "\"";
                                return r;
                            }
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

    LoweringResult VirLowering::lower(const vayu::Block& program) const {
        LoweringResult result;
        VirModule& m = result.module;
        m.name = "main";

        VirFunction mainFn;
        mainFn.name = "main";
        mainFn.returnType = { VirTypeKind::None };
        m.functions.push_back(std::move(mainFn));

        std::unordered_map<std::string, int> topLevelFunctions;
        for (const auto& stmt : program.stmts) {
            if (stmt->kind != vayu::StmtKind::Def) continue;
            const auto* d = static_cast<const vayu::DefStmt*>(stmt.get());
            VirFunction fn;
            fn.name = d->name;
            fn.returnType = { VirTypeKind::None };
            for (const auto& p : d->params) {
                fn.paramNames.push_back(p.name);
                fn.paramTypes.push_back({ VirTypeKind::Any });
            }
            int idx = (int)m.functions.size();
            m.functions.push_back(std::move(fn));
            topLevelFunctions[d->name] = idx;
        }

        for (const auto& stmt : program.stmts) {
            if (stmt->kind != vayu::StmtKind::Def) continue;
            const auto* d = static_cast<const vayu::DefStmt*>(stmt.get());
            int idx = topLevelFunctions[d->name];
            VirFunction& fn = m.functions[(size_t)idx];

            VirBlock entry;
            entry.label = "entry";

            LoweringContext fctx(m, fn, entry, result);
            fctx.topLevelFunctions = topLevelFunctions;
            for (const auto& pname : fn.paramNames) {
                int slot = fctx.nextLocal++;
                fctx.localsByName[pname] = slot;
            }
            // Registers and locals share slots at runtime. Params occupy
            // locals[0..N). Temporaries must start after them or Const/temps
            // overwrite parameter values (e.g. print("hi " + name) -> "hi hi ").
            fctx.nextReg = fctx.nextLocal;

            if (!fctx.lowerBlock(d->body)) {
                return result;
            }

            bool hasRet = !entry.instructions.empty() &&
                entry.instructions.back().opcode == VirOpcode::Ret;
            if (!hasRet) {
                int noneIdx = fctx.addNoneConst();
                int retReg = fctx.emitConst(noneIdx, vayu::SourceLocation{});
                VirInstruction ret;
                ret.opcode = VirOpcode::Ret;
                ret.operandReg = retReg;
                entry.instructions.push_back(ret);
            }

            fn.localCount = fctx.nextLocal;
            fn.blocks.push_back(std::move(entry));
            fn.entryBlock = 0;
        }

        VirFunction& mainRef = m.functions[0];
        VirBlock mainEntry;
        mainEntry.label = "entry";

        LoweringContext ctx(m, mainRef, mainEntry, result);
        ctx.topLevelFunctions = topLevelFunctions;

        for (const auto& stmt : program.stmts) {
            if (stmt->kind == vayu::StmtKind::Def) continue;
            if (stmt->kind == vayu::StmtKind::Pass) continue;
            if (stmt->kind == vayu::StmtKind::Block) {
                const auto* bs = static_cast<const vayu::BlockStmt*>(stmt.get());
                if (!ctx.lowerBlock(bs->body)) return result;
                continue;
            }
            int blocker = ctx.lowerStmt(stmt.get());
            if (blocker > 0) return result;
        }

        int noneIdx = ctx.addNoneConst();
        int retReg = ctx.emitConst(noneIdx, vayu::SourceLocation{});
        VirInstruction ret;
        ret.opcode = VirOpcode::Ret;
        ret.operandReg = retReg;
        mainEntry.instructions.push_back(ret);

        mainRef.localCount = ctx.nextLocal;
        mainRef.blocks.push_back(std::move(mainEntry));
        mainRef.entryBlock = 0;

        m.entryFunction = 0;
        result.success = true;
        return result;
    }

} // namespace vib