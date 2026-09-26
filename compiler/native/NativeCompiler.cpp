#include "NativeCompiler.hpp"
#include "VcbLower.hpp"
#include "parser/Parser.hpp"
#include "lexer/Lexer.hpp"
#include "VcbLower.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cstring>
#include <limits>
#include <filesystem>

#ifdef _WIN32
#  define _CRT_NONSTDC_NO_DEPRECATE
#  include <process.h>
#  define VAYU_GETPID() _getpid()
#else
#  include <unistd.h>
#  define VAYU_GETPID() getpid()
#endif

namespace vayu {

    NativeCompiler::NativeCompiler() {
#ifdef _WIN32
        qbePath_ = "tools\\qbe.exe";
        ccPath_ = "gcc";
        qbeTarget_ = "amd64_win";
#else
        qbePath_ = "tools/qbe";
        ccPath_ = "cc";
        qbeTarget_ = "amd64_sysv";
#endif

        if (const char* p = std::getenv("VAYU_QBE"))        qbePath_ = p;
        if (const char* p = std::getenv("VAYU_CC"))         ccPath_ = p;
        if (const char* p = std::getenv("VAYU_QBE_TARGET")) qbeTarget_ = p;

        // Relative default.  cmd.exe resolves "tools\\vcb.exe" against
        // the current working directory; the resolver
        // NativeCompiler::resolveVcbPath() walks up a few levels if the
        // cwd is not the project root.  A bare name like "vcb.exe" is
        // never stored here: cmd.exe would search PATH, fail with
        // "'vcb.exe' is not recognized", and hide the real problem.
#ifdef _WIN32
        vcbPath_ = "tools\\vcb.exe";
#else
        vcbPath_ = "tools/vcb";
#endif
        if (const char* p = std::getenv("VAYU_VCB")) {
            std::string s = p;
            if (s.find('\\') != std::string::npos ||
                s.find('/') != std::string::npos) {
                vcbPath_ = s;
            }
        }

        if (const char* p = std::getenv("VAYU_CC_OPT")) {
            int n = std::atoi(p);
            if (n >= 0 && n <= 3) optLevel_ = n;
        }

        /* Phase 25.0a - -O3.  The 25.0 benchmark showed native ~2.8x slower
           than hand-written C++ at -O2; -O3 recovers most of that on tight
           numeric loops.  Overridable via --opt / VAYU_CC_OPT. */
        optLevel_ = 3;
    }

    namespace {

        enum class VType {
            Int, Bool, Str, List, Map, Obj, Exc, Void, Unknown,
            Tuple, Set, Ptr, Float
        };

        struct VarInfo {
            VType       type = VType::Unknown;
            std::string clsName;
            VType       elemType = VType::Unknown;
            std::string elemClsName;
            VType       valType = VType::Unknown;
            std::vector<VType>       tupleElemTypes;
            std::vector<std::string> tupleElemClsNames;
        };

        struct ClassInfo {
            std::string                                       name;
            std::string                                       parentName;
            std::vector<std::string>                          fields;
            std::unordered_map<std::string, int>              fieldOffsets;
            int                                               totalSize = 0;
            const ClassStmt* decl = nullptr;
            std::unordered_map<std::string, const DefStmt*>   methods;
            const ClassInfo* parent = nullptr;
            bool                                              hasInit = false;
            const DefStmt* initDecl = nullptr;
            const ClassInfo* initOwner = nullptr;
        };

        static bool isExceptionName(const std::string& n) {
            return n == "Exception" ||
                n == "ValueError" || n == "TypeError" || n == "RuntimeError" ||
                n == "ZeroDivisionError" || n == "IndexError" || n == "KeyError" ||
                n == "NameError" || n == "AttributeError";
        }

        class QbeEmitter {
        public:
            std::string emit(const Block& program, const std::string& sourceDir) {
                sourceDir_ = sourceDir;
                loadImports(program);

                collectEnums(program);
                collectStatics(program);
                collectGenerators(program);
                collectClasses(program);
                for (auto& kv : modules_) collectClasses(kv.second);

                collectFunctions(program);

                collectStringLiterals(program);
                for (auto& kv : modules_) collectStringLiterals(kv.second);
                collectExceptionLiterals();

                emitCodeBody(program);

                std::string result;
                if (!globalData_.empty()) {
                    result += globalData_;
                    result += "\n";
                }
                if (!strLitData_.empty()) {
                    result += strLitData_;
                    result += "\n";
                }
                result += out_;
                out_.clear();
                return result;
            }

            void emitCodeBody(const Block& program) {
                resetFunctionState();
                raw("export function $vayu_main() {");
                raw("@start");

                std::unordered_set<std::string> topVars;
                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Def) continue;
                    collectVarsStmt(s.get(), topVars);
                }
                for (auto& s : program.stmts) {
                    if (s->kind != StmtKind::Class) continue;
                    auto* d = static_cast<const ClassStmt*>(s.get());
                    auto sit = statics_.find(d->name);
                    if (sit == statics_.end()) continue;
                    for (auto& kv : sit->second) topVars.insert(kv.second);
                }

                for (auto& n : topVars) {
                    std::string slot = "$" + mangle(n) + "_slot";
                    slots_[n] = slot;
                    globalData_ += "data " + slot + " = { l 0 }\n";
                }

                globalSlots_ = slots_;

                for (auto& kv : modules_)
                    emitModuleTopLevel(kv.first, kv.second);

                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Def) continue;
                    if (s->kind == StmtKind::Struct) continue;
                    if (s->kind == StmtKind::Enum) continue;
                    if (s->kind == StmtKind::Import || s->kind == StmtKind::FromImport) continue;
                    emitStmt(s.get());
                    if (terminated_) break;
                }

                globalVarInfo_ = varInfo_;

                if (!terminated_) line("ret");
                raw("}");
                raw("");

                for (auto& s : program.stmts) {
                    if (s->kind != StmtKind::Def) continue;
                    emitFunction(static_cast<const DefStmt*>(s.get()), nullptr, "");
                    raw("");
                }
                for (auto& kv : modules_) {
                    std::string prefix = mangle(kv.first) + "_";
                    currentModuleFnNames_.clear();
                    for (auto& s : kv.second.stmts) {
                        if (s->kind == StmtKind::Def)
                            currentModuleFnNames_.insert(
                                static_cast<const DefStmt*>(s.get())->name);
                    }
                    for (auto& s : kv.second.stmts) {
                        if (s->kind != StmtKind::Def) continue;
                        emitFunction(static_cast<const DefStmt*>(s.get()), nullptr, prefix);
                        raw("");
                    }
                    currentModuleFnNames_.clear();
                }

                for (auto& kv : classes_) {
                    const ClassInfo& ci = kv.second;
                    for (auto& m : ci.decl->methods) {
                        emitFunction(m.get(), &ci, "");
                        raw("");
                    }
                    emitClassCtor(ci);
                    raw("");
                }
            }

        private:
            std::string out_;
            int  nextTemp_ = 0;
            int  nextLabel_ = 0;
            bool terminated_ = false;

            std::unordered_map<std::string, std::string>    slots_;
            std::unordered_map<std::string, VarInfo>        varInfo_;

            std::unordered_map<std::string, std::string>    globalSlots_;
            std::unordered_map<std::string, VarInfo>        globalVarInfo_;

            std::string                                     currentClass_;
            std::vector<std::pair<std::string, std::string>> loopStack_;

            std::unordered_map<std::string, ClassInfo> classes_;
            std::unordered_map<std::string, int>       fieldGlobals_;
            int                                        nextFieldOffset_ = 0;
            std::unordered_map<std::string, const DefStmt*> topFnDecls_;
            std::unordered_map<std::string, const ExternFnDecl*> externDecls_;

            std::unordered_map<std::string,
                std::unordered_map<std::string, long long>> enums_;

            std::unordered_map<std::string,
                std::unordered_map<std::string, std::string>> statics_;

            std::unordered_set<std::string> generatorFunctions_;

            std::unordered_map<std::string, std::string> nonEscapingClasses_;

            std::string                                  strLitData_;
            std::unordered_map<std::string, std::string> strLitLabels_;
            int                                          nextStrLitId_ = 0;

            std::string                                  globalData_;

            std::string                                       sourceDir_;
            std::unordered_map<std::string, Block>            modules_;
            std::unordered_map<std::string, std::string>      fromImports_;
            std::string                                       currentModulePrefix_;
            std::unordered_set<std::string>                   currentModuleFnNames_;

            void resetFunctionState() {
                nextTemp_ = 0;
                nextLabel_ = 0;
                terminated_ = false;
                slots_.clear();
                varInfo_.clear();
                currentClass_.clear();
                loopStack_.clear();
                nonEscapingClasses_.clear();
            }

            std::string newTemp() { return "%t" + std::to_string(nextTemp_++); }
            std::string newLabel(const char* p) {
                return "@" + std::string(p) + std::to_string(nextLabel_++);
            }

            void line(const std::string& s) {
                out_ += "    " + s + "\n";
                if (s.rfind("ret", 0) == 0 ||
                    s.rfind("jmp", 0) == 0 ||
                    s.rfind("jnz", 0) == 0)
                    terminated_ = true;
                else
                    terminated_ = false;
            }
            void raw(const std::string& s) {
                if (!s.empty() && s[0] == '@' && !out_.empty() && out_.back() == '\n') {
                    size_t end = out_.size() - 1;
                    if (end > 0) {
                        size_t prevNl = out_.rfind('\n', end - 1);
                        size_t ls = (prevNl == std::string::npos) ? 0 : prevNl + 1;
                        if (ls < end && out_[ls] == '@') out_ += "    jmp " + s + "\n";
                    }
                }
                out_ += s + "\n";
                if (!s.empty() && s[0] == '@') terminated_ = false;
            }

            static std::string mangle(const std::string& s) {
                std::string r = "v";
                for (char c : s) {
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_')
                        r += c;
                    else
                        r += '_';
                }
                return r;
            }

            bool findModuleFile(const std::string& name, std::string& pathOut) const {
                if (!sourceDir_.empty()) {
                    std::string p = sourceDir_;
                    if (p.back() != '/' && p.back() != '\\') p += '/';
                    p += name + ".vyu";
                    std::ifstream in(p, std::ios::binary);
                    if (in) { pathOut = p; return true; }
                }
                {
                    std::string p = name + ".vyu";
                    std::ifstream in(p, std::ios::binary);
                    if (in) { pathOut = p; return true; }
                }
                if (const char* mp = std::getenv("VAYU_MODULE_PATH")) {
#ifdef _WIN32
                    const char sep = ';';
#else
                    const char sep = ':';
#endif
                    std::string s = mp;
                    size_t start = 0;
                    while (start <= s.size()) {
                        size_t end = s.find(sep, start);
                        if (end == std::string::npos) end = s.size();
                        std::string dir = s.substr(start, end - start);
                        if (!dir.empty()) {
                            if (dir.back() != '/' && dir.back() != '\\') dir += '/';
                            std::string p = dir + name + ".vyu";
                            std::ifstream in(p, std::ios::binary);
                            if (in) { pathOut = p; return true; }
                        }
                        if (end == s.size()) break;
                        start = end + 1;
                    }
                }
                return false;
            }

            void loadImports(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Import) {
                        auto* n = static_cast<const ImportStmt*>(s.get());
                        if (n->moduleName != "fs" && n->moduleName != "time" &&
                            n->moduleName != "json" && n->moduleName != "regex" &&
                            n->moduleName != "thread" && n->moduleName != "net" &&
                            n->moduleName != "crypto" && n->moduleName != "random" &&
                            n->moduleName != "os" && n->moduleName != "py" &&
                            n->moduleName != "gui" && n->moduleName != "raster" &&
                            n->moduleName != "tensor" && n->moduleName != "onnx" &&
                            n->moduleName != "cuda" && n->moduleName != "dml" &&
                            n->moduleName != "math" &&
                            !modules_.count(n->moduleName))
                            loadModule(n->moduleName, s->loc);
                    }
                    else if (s->kind == StmtKind::FromImport) {
                        auto* n = static_cast<const FromImportStmt*>(s.get());
                        if (n->moduleName != "fs" && n->moduleName != "time" &&
                            n->moduleName != "json" && n->moduleName != "regex" &&
                            n->moduleName != "thread" && n->moduleName != "net" &&
                            n->moduleName != "crypto" && n->moduleName != "random" &&
                            n->moduleName != "os" && n->moduleName != "gui" &&
                            !modules_.count(n->moduleName))
                            loadModule(n->moduleName, s->loc);
                        for (auto& item : n->items) {
                            const std::string& local = item.alias.empty() ? item.name : item.alias;
                            fromImports_[local] = n->moduleName;
                        }
                    }
                }
            }

            void loadModule(const std::string& name, SourceLocation loc) {
                (void)loc;
                std::string path;
                if (!findModuleFile(name, path))
                    throw std::runtime_error("native: cannot find module '" +
                        name + ".vyu'");
                std::ifstream in(path, std::ios::binary);
                if (!in) throw std::runtime_error("native: cannot open " + path);
                std::stringstream ss; ss << in.rdbuf();

                Block mod;
                try {
                    Lexer lexer(ss.str());
                    auto tokens = lexer.tokenize();
                    Parser parser(std::move(tokens));
                    mod = parser.parseProgram();
                }
                catch (const ParseError& e) {
                    throw std::runtime_error("native: parse error in module '" + name +
                        "' at line " + std::to_string(e.loc.line) +
                        ": " + e.what());
                }
                modules_[name] = std::move(mod);
                loadImports(modules_[name]);
            }

            void emitModuleTopLevel(const std::string& modName, const Block& modBlock) {
                std::string prefix = mangle(modName) + "_";

                std::unordered_set<std::string> modVars;
                for (auto& s : modBlock.stmts) {
                    if (s->kind == StmtKind::Def) continue;
                    collectVarsStmt(s.get(), modVars);
                }
                for (auto& n : modVars) {
                    std::string fullName = prefix + n;
                    std::string slot = "%" + fullName + "_slot";
                    slots_[fullName] = slot;
                    line(slot + " =l alloc8 8");
                    line("storel 0, " + slot);
                }

                currentModuleFnNames_.clear();
                for (auto& s : modBlock.stmts) {
                    if (s->kind == StmtKind::Def)
                        currentModuleFnNames_.insert(
                            static_cast<const DefStmt*>(s.get())->name);
                }
                currentModulePrefix_ = prefix;
                for (auto& s : modBlock.stmts) {
                    if (s->kind == StmtKind::Def) continue;
                    if (s->kind == StmtKind::Struct || s->kind == StmtKind::Class) continue;
                    if (s->kind == StmtKind::Enum) continue;
                    if (s->kind == StmtKind::Import || s->kind == StmtKind::FromImport) continue;
                    emitStmt(s.get());
                }
                currentModulePrefix_.clear();
                currentModuleFnNames_.clear();
            }

            std::string internString(const std::string& s) {
                auto it = strLitLabels_.find(s);
                if (it != strLitLabels_.end()) return it->second;

                std::string label = "$vayu_strlit_" + std::to_string(nextStrLitId_++);
                strLitLabels_[s] = label;

                std::ostringstream d;
                d << "data " << label << " = { l " << s.size() << ", ";
                std::string run;
                auto flushRun = [&]() {
                    if (run.empty()) return;
                    d << "b \"" << run << "\", ";
                    run.clear();
                    };
                for (unsigned char c : s) {
                    if (c >= 32 && c < 127 && c != '"' && c != '\\') run += (char)c;
                    else { flushRun(); d << "b " << (int)c << ", "; }
                }
                flushRun();
                d << "b 0 }";
                strLitData_ += d.str() + "\n";
                return label;
            }

            void collectStringLiterals(const Block& b) {
                for (auto& s : b.stmts) collectStringLiteralsStmt(s.get());
            }
            void collectStringLiteralsStmt(const Stmt* s) {
                if (!s) return;
                switch (s->kind) {
                case StmtKind::Expr:
                    collectStringLiteralsExpr(static_cast<const ExprStmt*>(s)->expr.get());
                    break;
                case StmtKind::Assign: {
                    auto* n = static_cast<const AssignStmt*>(s);
                    collectStringLiteralsExpr(n->target.get());
                    collectStringLiteralsExpr(n->value.get());
                    break;
                }
                case StmtKind::AnnotAssign: {
                    auto* n = static_cast<const AnnotAssignStmt*>(s);
                    if (n->type)  collectStringLiteralsExpr(n->type.get());
                    if (n->value) collectStringLiteralsExpr(n->value.get());
                    break;
                }
                case StmtKind::Const: {
                    auto* n = static_cast<const ConstStmt*>(s);
                    if (n->type)  collectStringLiteralsExpr(n->type.get());
                    if (n->value) collectStringLiteralsExpr(n->value.get());
                    break;
                }
                case StmtKind::Yield: {
                    auto* n = static_cast<const YieldStmt*>(s);
                    if (n->value) collectStringLiteralsExpr(n->value.get());
                    break;
                }
                case StmtKind::If: {
                    auto* n = static_cast<const IfStmt*>(s);
                    collectStringLiteralsExpr(n->cond.get());
                    collectStringLiterals(n->thenBody);
                    for (auto& e : n->elifs) {
                        collectStringLiteralsExpr(e.cond.get());
                        collectStringLiterals(e.body);
                    }
                    if (n->elseBody) collectStringLiterals(*n->elseBody);
                    break;
                }
                case StmtKind::While: {
                    auto* n = static_cast<const WhileStmt*>(s);
                    collectStringLiteralsExpr(n->cond.get());
                    collectStringLiterals(n->body);
                    break;
                }
                case StmtKind::For: {
                    auto* n = static_cast<const ForStmt*>(s);
                    collectStringLiteralsExpr(n->iterable.get());
                    collectStringLiterals(n->body);
                    break;
                }
                case StmtKind::Def: {
                    auto* n = static_cast<const DefStmt*>(s);
                    collectStringLiterals(n->body);
                    break;
                }
                case StmtKind::Return: {
                    auto* n = static_cast<const ReturnStmt*>(s);
                    if (n->value) collectStringLiteralsExpr(n->value.get());
                    break;
                }
                case StmtKind::Class: {
                    auto* n = static_cast<const ClassStmt*>(s);
                    for (auto& sf : n->staticFields) {
                        if (sf.init) collectStringLiteralsExpr(sf.init.get());
                    }
                    for (auto& m : n->methods) collectStringLiterals(m->body);
                    break;
                }
                case StmtKind::Try: {
                    auto* n = static_cast<const TryStmt*>(s);
                    collectStringLiterals(n->tryBody);
                    for (auto& h : n->handlers) collectStringLiterals(h.body);
                    if (n->finallyBody) collectStringLiterals(*n->finallyBody);
                    break;
                }
                case StmtKind::Raise: {
                    auto* n = static_cast<const RaiseStmt*>(s);
                    if (n->exception) collectStringLiteralsExpr(n->exception.get());
                    break;
                }
                case StmtKind::Block: {
                    auto* n = static_cast<const BlockStmt*>(s);
                    collectStringLiterals(n->body);
                    break;
                }
                case StmtKind::Extern: break;
                default: break;
                }
            }
            void collectStringLiteralsExpr(const Expr* e) {
                if (!e) return;
                switch (e->kind) {
                case ExprKind::StringLit: {
                    auto* n = static_cast<const StringLitExpr*>(e);
                    internString(n->value);
                    break;
                }
                case ExprKind::Binary: {
                    auto* n = static_cast<const BinaryExpr*>(e);
                    collectStringLiteralsExpr(n->lhs.get());
                    collectStringLiteralsExpr(n->rhs.get());
                    break;
                }
                case ExprKind::Unary: {
                    auto* n = static_cast<const UnaryExpr*>(e);
                    collectStringLiteralsExpr(n->operand.get());
                    break;
                }
                case ExprKind::Grouping: {
                    auto* n = static_cast<const GroupingExpr*>(e);
                    collectStringLiteralsExpr(n->inner.get());
                    break;
                }
                case ExprKind::Call: {
                    auto* n = static_cast<const CallExpr*>(e);
                    collectStringLiteralsExpr(n->callee.get());
                    for (auto& a : n->args) collectStringLiteralsExpr(a.value.get());
                    break;
                }
                case ExprKind::Index: {
                    auto* n = static_cast<const IndexExpr*>(e);
                    collectStringLiteralsExpr(n->target.get());
                    collectStringLiteralsExpr(n->index.get());
                    break;
                }
                case ExprKind::Attr: {
                    auto* n = static_cast<const AttrExpr*>(e);
                    collectStringLiteralsExpr(n->target.get());
                    break;
                }
                case ExprKind::Lambda: {
                    auto* n = static_cast<const LambdaExpr*>(e);
                    collectStringLiteralsExpr(n->body.get());
                    break;
                }
                case ExprKind::ListLit: {
                    auto* n = static_cast<const ListLitExpr*>(e);
                    for (auto& el : n->elements) collectStringLiteralsExpr(el.get());
                    break;
                }
                case ExprKind::MapLit: {
                    auto* n = static_cast<const MapLitExpr*>(e);
                    for (auto& en : n->entries) {
                        collectStringLiteralsExpr(en.key.get());
                        collectStringLiteralsExpr(en.value.get());
                    }
                    break;
                }
                case ExprKind::Slice: {
                    auto* n = static_cast<const SliceExpr*>(e);
                    collectStringLiteralsExpr(n->target.get());
                    if (n->start) collectStringLiteralsExpr(n->start.get());
                    if (n->end)   collectStringLiteralsExpr(n->end.get());
                    break;
                }
                default: break;
                }
            }

            void collectExceptionLiterals() {
                internString("Exception");
                internString("ValueError");
                internString("TypeError");
                internString("RuntimeError");
                internString("ZeroDivisionError");
                internString("IndexError");
                internString("KeyError");
                internString("NameError");
                internString("AttributeError");
            }

            void collectFunctions(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Def) {
                        auto* d = static_cast<const DefStmt*>(s.get());
                        topFnDecls_[d->name] = d;
                    }
                    else if (s->kind == StmtKind::Extern) {
                        auto* ex = static_cast<const ExternBlockStmt*>(s.get());
                        for (auto& fn : ex->funcs)
                            externDecls_[fn.name] = &fn;
                    }
                }
            }

            void collectEnums(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind != StmtKind::Enum) continue;
                    auto* d = static_cast<const EnumStmt*>(s.get());
                    std::unordered_map<std::string, long long> items;
                    for (auto& it : d->items) {
                        long long v = 0;
                        if (it.value && it.value->kind == ExprKind::IntLit)
                            v = static_cast<const IntLitExpr*>(it.value.get())->value;
                        items[it.name] = v;
                    }
                    enums_[d->name] = std::move(items);
                }
            }

            void collectStatics(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind != StmtKind::Class) continue;
                    auto* d = static_cast<const ClassStmt*>(s.get());
                    std::unordered_map<std::string, std::string> sm;
                    for (auto& sf : d->staticFields)
                        sm[sf.name] = "__static_" + d->name + "__" + sf.name;
                    statics_[d->name] = std::move(sm);
                }
            }

            void collectGenerators(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind != StmtKind::Def) continue;
                    auto* d = static_cast<const DefStmt*>(s.get());
                    if (d->isGenerator) generatorFunctions_.insert(d->name);
                }
            }

            void collectClasses(const Block& program) {
                for (auto& s : program.stmts) {
                    if (s->kind == StmtKind::Class) {
                        auto* n = static_cast<const ClassStmt*>(s.get());
                        for (auto& f : n->fields) allocFieldOffset(f.name);
                        allocFieldOffset("typeName");
                        allocFieldOffset("message");
                    }
                    else if (s->kind == StmtKind::Struct) {
                        auto* n = static_cast<const StructStmt*>(s.get());
                        for (auto& f : n->fields) allocFieldOffset(f.name);
                    }
                }
                for (auto& stmt : program.stmts) {
                    if (stmt->kind != StmtKind::Class) continue;
                    auto* n = static_cast<const ClassStmt*>(stmt.get());
                    ClassInfo ci;
                    ci.name = n->name;
                    ci.parentName = n->parentName;
                    ci.decl = n;
                    for (auto& f : n->fields) ci.fields.push_back(f.name);
                    for (auto& m : n->methods) {
                        if (m->name == "__init__") {
                            ci.hasInit = true;
                            ci.initDecl = m.get();
                            continue;
                        }
                        ci.methods[m->name] = m.get();
                    }
                    classes_[n->name] = std::move(ci);
                }
                for (auto it = classes_.begin(); it != classes_.end(); ++it) {
                    ClassInfo& ci = it->second;
                    if (ci.parentName.empty()) continue;
                    auto pit = classes_.find(ci.parentName);
                    if (pit != classes_.end()) ci.parent = &pit->second;
                }
                std::unordered_map<std::string, std::vector<std::string>> ownFields;
                for (auto it = classes_.begin(); it != classes_.end(); ++it)
                    ownFields[it->first] = it->second.fields;

                for (int pass = 0; pass < 8; ++pass) {
                    bool changed = false;
                    for (auto it = classes_.begin(); it != classes_.end(); ++it) {
                        ClassInfo& ci = it->second;
                        std::vector<std::string> rebuilt;
                        if (ci.parent)
                            for (auto& f : ci.parent->fields) rebuilt.push_back(f);
                        for (auto& f : ownFields[ci.name]) {
                            bool dup = false;
                            for (auto& r : rebuilt) if (r == f) { dup = true; break; }
                            if (!dup) rebuilt.push_back(f);
                        }
                        if (rebuilt != ci.fields) {
                            ci.fields = std::move(rebuilt);
                            changed = true;
                        }
                    }
                    if (!changed) break;
                }
                for (auto it = classes_.begin(); it != classes_.end(); ++it) {
                    ClassInfo& ci = it->second;
                    if (ci.hasInit && !ci.initOwner) ci.initOwner = &ci;
                }
                for (int pass = 0; pass < 8; ++pass) {
                    bool changed = false;
                    for (auto it = classes_.begin(); it != classes_.end(); ++it) {
                        ClassInfo& ci = it->second;
                        if (ci.hasInit || !ci.parent || !ci.parent->hasInit) continue;
                        ci.hasInit = true;
                        ci.initDecl = ci.parent->initDecl;
                        ci.initOwner = ci.parent->initOwner;
                        changed = true;
                    }
                    if (!changed) break;
                }
                for (auto it = classes_.begin(); it != classes_.end(); ++it) {
                    ClassInfo& ci = it->second;
                    for (auto& f : ci.fields) ci.fieldOffsets[f] = fieldGlobals_.at(f);
                    int maxOff = -8;
                    for (auto& kv : ci.fieldOffsets)
                        if (kv.second > maxOff) maxOff = kv.second;
                    ci.totalSize = maxOff + 8;
                }
            }

            int allocFieldOffset(const std::string& name) {
                auto it = fieldGlobals_.find(name);
                if (it != fieldGlobals_.end()) return it->second;
                int off = nextFieldOffset_;
                fieldGlobals_[name] = off;
                nextFieldOffset_ += 8;
                return off;
            }

            const ClassInfo* findClass(const std::string& n) const {
                auto it = classes_.find(n);
                return it == classes_.end() ? nullptr : &it->second;
            }

            const ClassInfo* findClassDefiningMethod(const ClassInfo* ci,
                const std::string& name) const {
                for (auto c = ci; c; c = c->parent) {
                    if (name == "__init__") {
                        if (c->hasInit && c->initOwner == c) return c;
                    }
                    else {
                        if (c->methods.count(name)) return c;
                    }
                }
                return nullptr;
            }

            static void collectVarsStmt(const Stmt* s, std::unordered_set<std::string>& out);
            static void collectVarsBlock(const Block& b, std::unordered_set<std::string>& out);

            struct Val {
                std::string ssa;
                VType       type = VType::Unknown;
                std::string cls;
                VType       elemType = VType::Unknown;
                std::string elemCls;
                VType       valType = VType::Unknown;
                std::vector<VType>       tupleElemTypes;
                std::vector<std::string> tupleElemClsNames;
            };

            static int kindOf(VType t) {
                switch (t) {
                case VType::Int:   return 0;
                case VType::Bool:  return 1;
                case VType::Str:   return 2;
                case VType::Float: return 7;
                default:           return 0;
                }
            }

            static int tagOf(VType t) {
                switch (t) {
                case VType::Int:   return 0;
                case VType::Bool:  return 1;
                case VType::Str:   return 2;
                case VType::List:  return 3;
                case VType::Map:   return 4;
                case VType::Tuple: return 5;
                case VType::Set:   return 6;
                case VType::Float: return 7;
                default:           return 0;
                }
            }

            void inferFromAnnotation(const Expr* e, Val& v) {
                if (!e) return;
                if (e->kind == ExprKind::NameRef) {
                    const auto* n = static_cast<const NameRefExpr*>(e);
                    const std::string& s = n->name;
                    if (s == "int")   v.type = VType::Int;
                    else if (s == "bool")  v.type = VType::Bool;
                    else if (s == "str")   v.type = VType::Str;
                    else if (s == "float") v.type = VType::Float;
                    else if (s == "list")  v.type = VType::List;
                    else if (s == "map")   v.type = VType::Map;
                    else if (classes_.count(s)) {
                        v.type = VType::Obj;
                        v.cls = s;
                    }
                    return;
                }
                if (e->kind == ExprKind::GenericType) {
                    const auto* g = static_cast<const GenericTypeExpr*>(e);
                    if (g->name == "list") {
                        v.type = VType::List;
                        if (!g->typeArgs.empty()) {
                            Val inner;
                            inferFromAnnotation(g->typeArgs[0].get(), inner);
                            v.elemType = inner.type;
                            v.elemCls = inner.cls;
                        }
                        return;
                    }
                    if (g->name == "map") {
                        v.type = VType::Map;
                        if (g->typeArgs.size() >= 2) {
                            Val kk, vv;
                            inferFromAnnotation(g->typeArgs[0].get(), kk);
                            inferFromAnnotation(g->typeArgs[1].get(), vv);
                            v.elemType = kk.type;
                            v.elemCls = kk.cls;
                            v.valType = vv.type;
                        }
                        return;
                    }
                    // Phase 15.2: ptr<T> / ref<T> erased to T.
                    if (g->name == "ptr" || g->name == "ref") {
                        if (!g->typeArgs.empty())
                            inferFromAnnotation(g->typeArgs[0].get(), v);
                        return;
                    }
                    if (g->name == "ptr") {
                        v.type = VType::Ptr;
                        return;
                    }
                    if (g->name == "ref") {
                        if (!g->typeArgs.empty())
                            inferFromAnnotation(g->typeArgs[0].get(), v);
                        return;
                    }
                    if (g->name == "unique" || g->name == "shared" ||
                        g->name == "weak") {
                        if (!g->typeArgs.empty())
                            inferFromAnnotation(g->typeArgs[0].get(), v);
                        return;
                    }
                }
            }

            static bool isSafeAttrTarget(const Expr* target, const std::string& varName) {
                if (!target) return false;
                if (target->kind != ExprKind::NameRef) return false;
                return static_cast<const NameRefExpr*>(target)->name == varName;
            }

            bool exprEscapes(const Expr* e, const std::string& varName) {
                if (!e) return false;
                switch (e->kind) {
                case ExprKind::NameRef:
                    return static_cast<const NameRefExpr*>(e)->name == varName;

                case ExprKind::Attr: {
                    auto* n = static_cast<const AttrExpr*>(e);
                    if (isSafeAttrTarget(n->target.get(), varName)) return false;
                    return exprEscapes(n->target.get(), varName);
                }

                case ExprKind::Call: {
                    auto* n = static_cast<const CallExpr*>(e);
                    if (n->callee->kind == ExprKind::Attr) {
                        auto* a = static_cast<const AttrExpr*>(n->callee.get());
                        if (isSafeAttrTarget(a->target.get(), varName)) {
                            for (auto& arg : n->args) {
                                if (exprEscapes(arg.value.get(), varName)) return true;
                            }
                            return false;
                        }
                    }
                    if (n->callee->kind == ExprKind::NameRef &&
                        static_cast<const NameRefExpr*>(n->callee.get())->name == varName) {
                        return true;
                    }
                    if (exprEscapes(n->callee.get(), varName)) return true;
                    for (auto& arg : n->args) {
                        if (exprEscapes(arg.value.get(), varName)) return true;
                    }
                    return false;
                }

                case ExprKind::Binary: {
                    auto* n = static_cast<const BinaryExpr*>(e);
                    return exprEscapes(n->lhs.get(), varName) ||
                        exprEscapes(n->rhs.get(), varName);
                }

                case ExprKind::Unary:
                    return exprEscapes(static_cast<const UnaryExpr*>(e)->operand.get(), varName);

                case ExprKind::Grouping:
                    return exprEscapes(static_cast<const GroupingExpr*>(e)->inner.get(), varName);

                case ExprKind::Index: {
                    auto* n = static_cast<const IndexExpr*>(e);
                    if (isSafeAttrTarget(n->target.get(), varName)) return true;
                    return exprEscapes(n->target.get(), varName) ||
                        exprEscapes(n->index.get(), varName);
                }

                case ExprKind::ListLit: {
                    auto* n = static_cast<const ListLitExpr*>(e);
                    for (auto& el : n->elements) {
                        if (exprEscapes(el.get(), varName)) return true;
                    }
                    return false;
                }

                case ExprKind::MapLit: {
                    auto* n = static_cast<const MapLitExpr*>(e);
                    for (auto& en : n->entries) {
                        if (exprEscapes(en.key.get(), varName)) return true;
                        if (exprEscapes(en.value.get(), varName)) return true;
                    }
                    return false;
                }

                case ExprKind::Lambda: {
                    auto* n = static_cast<const LambdaExpr*>(e);
                    return exprEscapes(n->body.get(), varName);
                }
                case ExprKind::Slice: {
                    auto* n = static_cast<const SliceExpr*>(e);
                    if (exprEscapes(n->target.get(), varName)) return true;
                    if (n->start && exprEscapes(n->start.get(), varName)) return true;
                    if (n->end && exprEscapes(n->end.get(), varName)) return true;
                    return false;
                }
                default:
                    return false;
                }
            }

            bool stmtEscapes(const Stmt* s, const std::string& varName) {
                if (!s) return false;
                switch (s->kind) {
                case StmtKind::Expr:
                    return exprEscapes(static_cast<const ExprStmt*>(s)->expr.get(), varName);

                case StmtKind::Assign: {
                    auto* n = static_cast<const AssignStmt*>(s);
                    if (n->target->kind == ExprKind::NameRef &&
                        static_cast<const NameRefExpr*>(n->target.get())->name == varName) {
                        return true;
                    }
                    if (n->target->kind == ExprKind::Attr) {
                        auto* a = static_cast<const AttrExpr*>(n->target.get());
                        if (isSafeAttrTarget(a->target.get(), varName)) {
                            return exprEscapes(n->value.get(), varName);
                        }
                    }
                    if (n->target->kind == ExprKind::Index) {
                        auto* ix = static_cast<const IndexExpr*>(n->target.get());
                        if (isSafeAttrTarget(ix->target.get(), varName)) return true;
                    }
                    if (exprEscapes(n->target.get(), varName)) return true;
                    return exprEscapes(n->value.get(), varName);
                }

                case StmtKind::AnnotAssign: {
                    auto* n = static_cast<const AnnotAssignStmt*>(s);
                    if (n->name == varName) return true;
                    if (n->value) return exprEscapes(n->value.get(), varName);
                    return false;
                }

                case StmtKind::Const: {
                    auto* n = static_cast<const ConstStmt*>(s);
                    if (n->name == varName) return true;
                    if (n->value) return exprEscapes(n->value.get(), varName);
                    return false;
                }

                case StmtKind::Yield: {
                    auto* n = static_cast<const YieldStmt*>(s);
                    if (n->value) return exprEscapes(n->value.get(), varName);
                    return false;
                }

                case StmtKind::If: {
                    auto* n = static_cast<const IfStmt*>(s);
                    if (exprEscapes(n->cond.get(), varName)) return true;
                    if (blockEscapes(n->thenBody, varName)) return true;
                    for (auto& ec : n->elifs) {
                        if (exprEscapes(ec.cond.get(), varName)) return true;
                        if (blockEscapes(ec.body, varName)) return true;
                    }
                    if (n->elseBody && blockEscapes(*n->elseBody, varName)) return true;
                    return false;
                }

                case StmtKind::While: {
                    auto* n = static_cast<const WhileStmt*>(s);
                    if (exprEscapes(n->cond.get(), varName)) return true;
                    return blockEscapes(n->body, varName);
                }

                case StmtKind::For: {
                    auto* n = static_cast<const ForStmt*>(s);
                    if (n->targetName == varName) return true;
                    if (exprEscapes(n->iterable.get(), varName)) return true;
                    return blockEscapes(n->body, varName);
                }

                case StmtKind::Return: {
                    auto* n = static_cast<const ReturnStmt*>(s);
                    if (n->value) return exprEscapes(n->value.get(), varName);
                    return false;
                }

                case StmtKind::Try: {
                    auto* n = static_cast<const TryStmt*>(s);
                    if (blockEscapes(n->tryBody, varName)) return true;
                    for (auto& h : n->handlers) {
                        if (blockEscapes(h.body, varName)) return true;
                    }
                    if (n->finallyBody && blockEscapes(*n->finallyBody, varName)) return true;
                    return false;
                }

                case StmtKind::Raise: {
                    auto* n = static_cast<const RaiseStmt*>(s);
                    if (n->exception) return exprEscapes(n->exception.get(), varName);
                    return false;
                }

                default:
                    return false;
                }
            }

            bool blockEscapes(const Block& b, const std::string& varName) {
                for (auto& s : b.stmts) {
                    if (stmtEscapes(s.get(), varName)) return true;
                }
                return false;
            }

            void findCandidates(const Block& b,
                std::unordered_map<std::string, std::string>& out,
                std::unordered_map<std::string, int>& counts) {
                for (auto& s : b.stmts) findCandidatesStmt(s.get(), out, counts);
            }

            void findCandidatesStmt(const Stmt* s,
                std::unordered_map<std::string, std::string>& out,
                std::unordered_map<std::string, int>& counts) {
                if (!s) return;
                if (s->kind == StmtKind::Assign) {
                    auto* n = static_cast<const AssignStmt*>(s);
                    if (n->target->kind == ExprKind::NameRef &&
                        n->value->kind == ExprKind::Call) {
                        auto* c = static_cast<const CallExpr*>(n->value.get());
                        if (c->callee->kind == ExprKind::NameRef) {
                            const std::string& lhs = static_cast<const NameRefExpr*>(
                                n->target.get())->name;
                            const std::string& cname = static_cast<const NameRefExpr*>(
                                c->callee.get())->name;
                            counts[lhs]++;
                            if (classes_.count(cname)) {
                                const ClassInfo* ci = findClass(cname);
                                if (ci && ci->hasInit) {
                                    out[lhs] = cname;
                                }
                            }
                        }
                    }
                }
                switch (s->kind) {
                case StmtKind::If: {
                    auto* n = static_cast<const IfStmt*>(s);
                    findCandidates(n->thenBody, out, counts);
                    for (auto& ec : n->elifs) findCandidates(ec.body, out, counts);
                    if (n->elseBody) findCandidates(*n->elseBody, out, counts);
                    break;
                }
                case StmtKind::While:
                    findCandidates(static_cast<const WhileStmt*>(s)->body, out, counts);
                    break;
                case StmtKind::For:
                    findCandidates(static_cast<const ForStmt*>(s)->body, out, counts);
                    break;
                case StmtKind::Try: {
                    auto* n = static_cast<const TryStmt*>(s);
                    findCandidates(n->tryBody, out, counts);
                    for (auto& h : n->handlers) findCandidates(h.body, out, counts);
                    if (n->finallyBody) findCandidates(*n->finallyBody, out, counts);
                    break;
                }
                case StmtKind::Block: {
                    auto* n = static_cast<const BlockStmt*>(s);
                    findCandidates(n->body, out, counts);
                    break;
                }
                case StmtKind::Extern: break;
                default: break;
                }
            }

            void analyzeEscapes(const Block& body) {
                nonEscapingClasses_.clear();

                std::unordered_map<std::string, std::string> candidateClass;
                std::unordered_map<std::string, int>         assignCount;
                findCandidates(body, candidateClass, assignCount);

                for (auto& kv : candidateClass) {
                    const std::string& varName = kv.first;
                    const std::string& className = kv.second;
                    if (assignCount[varName] != 1) continue;
                    if (blockEscapes(body, varName)) continue;
                    const ClassInfo* ci = findClass(className);
                    if (!ci) continue;
                    nonEscapingClasses_[varName] = className;
                }
            }

            Val inferExprType(const Expr* e) {
                Val r;
                if (!e) return r;
                switch (e->kind) {
                case ExprKind::IntLit:   r.type = VType::Int;  break;
                case ExprKind::FloatLit: r.type = VType::Float; break;
                case ExprKind::BoolLit:  r.type = VType::Bool; break;
                case ExprKind::NoneLit:  r.type = VType::Int;  break;
                case ExprKind::StringLit:
                case ExprKind::CharLit:  r.type = VType::Str;  break;
                case ExprKind::NameRef: {
                    auto* n = static_cast<const NameRefExpr*>(e);
                    auto it = varInfo_.find(n->name);
                    if (it != varInfo_.end()) {
                        r.type = it->second.type;
                        r.cls = it->second.clsName;
                        r.elemType = it->second.elemType;
                        r.elemCls = it->second.elemClsName;
                        r.valType = it->second.valType;
                    }
                    else {
                        auto git = globalVarInfo_.find(n->name);
                        if (git != globalVarInfo_.end()) {
                            r.type = git->second.type;
                            r.cls = git->second.clsName;
                            r.elemType = git->second.elemType;
                            r.elemCls = git->second.elemClsName;
                            r.valType = git->second.valType;
                        }
                    }
                    break;
                }
                case ExprKind::ListLit: {
                    auto* n = static_cast<const ListLitExpr*>(e);
                    r.type = VType::List;
                    if (!n->elements.empty()) {
                        Val f = inferExprType(n->elements[0].get());
                        r.elemType = f.type;
                        r.elemCls = f.cls;
                    }
                    break;
                }
                case ExprKind::MapLit: {
                    auto* n = static_cast<const MapLitExpr*>(e);
                    r.type = VType::Map;
                    if (!n->entries.empty()) {
                        Val vv = inferExprType(n->entries[0].value.get());
                        r.valType = vv.type;
                    }
                    break;
                }
                case ExprKind::Call: {
                    auto* c = static_cast<const CallExpr*>(e);
                    if (c->callee->kind == ExprKind::NameRef) {
                        const auto* nm = static_cast<const NameRefExpr*>(
                            c->callee.get());
                        if (classes_.count(nm->name)) {
                            r.type = VType::Obj;
                            r.cls = nm->name;
                            return r;
                        }
                        if (nm->name == "enumerate" || nm->name == "zip") {
                            r.type = VType::List;
                            r.elemType = VType::List;
                            return r;
                        }
                        auto fit = topFnDecls_.find(nm->name);
                        if (fit != topFnDecls_.end() && fit->second->returnType) {
                            bindTypeParamsFromCall(fit->second, c, r);
                        }
                    }
                    break;
                }
                case ExprKind::Grouping:
                    return inferExprType(
                        static_cast<const GroupingExpr*>(e)->inner.get());
                case ExprKind::Slice:
                    return inferExprType(
                        static_cast<const SliceExpr*>(e)->target.get());
                default: break;
                }
                return r;
            }

            void bindTypeParamsFromCall(const DefStmt* def,
                const CallExpr* call,
                Val& r) {
                if (!def) return;
                if (def->typeParams.empty()) {
                    if (def->returnType)
                        inferFromAnnotation(def->returnType.get(), r);
                    return;
                }
                std::unordered_map<std::string, Val> subst;

                auto isTypeParam = [&](const std::string& name) -> bool {
                    for (auto& tp : def->typeParams) if (tp == name) return true;
                    return false;
                    };
                auto bindParam = [&](const std::string& name,
                    const Val& val) {
                        if (val.type == VType::Unknown) return;
                        if (subst.find(name) != subst.end()) return;
                        subst[name] = val;
                    };

                auto unify = [&](auto&& self, const Expr* ann,
                    const Val& actual,
                    const Expr* actualExpr) -> void {
                        if (!ann) return;
                        if (ann->kind == ExprKind::NameRef) {
                            const std::string& n =
                                static_cast<const NameRefExpr*>(ann)->name;
                            if (isTypeParam(n)) { bindParam(n, actual); return; }
                            const ClassInfo* ci = findClass(n);
                            if (!ci || !ci->decl ||
                                ci->decl->typeParams.empty()) return;
                            if (!actualExpr || actualExpr->kind != ExprKind::Call)
                                return;
                            auto* cc = static_cast<const CallExpr*>(actualExpr);
                            if (cc->callee->kind != ExprKind::NameRef) return;
                            if (static_cast<const NameRefExpr*>(
                                cc->callee.get())->name != n) return;
                            for (size_t fi = 0;
                                fi < ci->decl->fields.size() &&
                                fi < cc->args.size(); ++fi) {
                                const Expr* fieldAnn =
                                    ci->decl->fields[fi].type.get();
                                if (!fieldAnn ||
                                    fieldAnn->kind != ExprKind::NameRef) continue;
                                const std::string& fname =
                                    static_cast<const NameRefExpr*>(fieldAnn)->name;
                                bool isClassTP = false;
                                for (auto& ctp : ci->decl->typeParams)
                                    if (ctp == fname) { isClassTP = true; break; }
                                if (!isClassTP) continue;
                                Val argVal = inferExprType(
                                    cc->args[fi].value.get());
                                if (isTypeParam(fname)) bindParam(fname, argVal);
                            }
                            return;
                        }
                        if (ann->kind == ExprKind::GenericType) {
                            const auto* g =
                                static_cast<const GenericTypeExpr*>(ann);
                            if (g->name == "list" && !g->typeArgs.empty()) {
                                Val inner;
                                inner.type = actual.elemType;
                                inner.cls = actual.elemCls;
                                self(self, g->typeArgs[0].get(), inner, nullptr);
                            }
                            else if (g->name == "map" && g->typeArgs.size() >= 2) {
                                Val kk; kk.type = actual.elemType;
                                kk.cls = actual.elemCls;
                                Val vv; vv.type = actual.valType;
                                self(self, g->typeArgs[0].get(), kk, nullptr);
                                self(self, g->typeArgs[1].get(), vv, nullptr);
                            }
                        }
                    };

                for (size_t i = 0;
                    i < call->args.size() && i < def->params.size(); ++i) {
                    Val a = inferExprType(call->args[i].value.get());
                    unify(unify, def->params[i].type.get(), a,
                        call->args[i].value.get());
                }

                applySubst(def->returnType.get(), subst, r,
                    def->typeParams);
            }

            void applySubst(const Expr* ann,
                const std::unordered_map<std::string, Val>& subst,
                Val& out,
                const std::vector<std::string>& typeParams) {
                if (!ann) return;
                if (ann->kind == ExprKind::NameRef) {
                    const std::string& n =
                        static_cast<const NameRefExpr*>(ann)->name;
                    for (auto& tp : typeParams) {
                        if (tp == n) {
                            auto it = subst.find(n);
                            if (it != subst.end()) {
                                out.type = it->second.type;
                                out.cls = it->second.cls;
                                out.elemType = it->second.elemType;
                                out.elemCls = it->second.elemCls;
                                out.valType = it->second.valType;
                            }
                            return;
                        }
                    }
                    inferFromAnnotation(ann, out);
                    return;
                }
                if (ann->kind == ExprKind::GenericType) {
                    const auto* g = static_cast<const GenericTypeExpr*>(ann);
                    if (g->name == "list") {
                        out.type = VType::List;
                        if (!g->typeArgs.empty()) {
                            Val inner;
                            applySubst(g->typeArgs[0].get(), subst, inner, typeParams);
                            out.elemType = inner.type;
                            out.elemCls = inner.cls;
                        }
                    }
                    else if (g->name == "map") {
                        out.type = VType::Map;
                        if (g->typeArgs.size() >= 2) {
                            Val kk, vv;
                            applySubst(g->typeArgs[0].get(), subst, kk, typeParams);
                            applySubst(g->typeArgs[1].get(), subst, vv, typeParams);
                            out.elemType = kk.type;
                            out.elemCls = kk.cls;
                            out.valType = vv.type;
                        }
                    }
                }
            }

            void inferTypeFromExpr(const Expr* e, Val& v) {
                if (!e) return;
                if (e->kind == ExprKind::NameRef) {
                    auto it = varInfo_.find(static_cast<const NameRefExpr*>(e)->name);
                    if (it != varInfo_.end()) {
                        v.type = it->second.type;
                        v.cls = it->second.clsName;
                        v.elemType = it->second.elemType;
                        v.elemCls = it->second.elemClsName;
                        v.valType = it->second.valType;
                    }
                }
            }

            Val emitExpr(const Expr* e) {
                Val r;
                if (!e) { r.ssa = "0"; r.type = VType::Int; return r; }

                switch (e->kind) {
                case ExprKind::IntLit: {
                    auto* n = static_cast<const IntLitExpr*>(e);
                    r.ssa = std::to_string(n->value);
                    r.type = VType::Int;
                    return r;
                }
                case ExprKind::BoolLit: {
                    auto* n = static_cast<const BoolLitExpr*>(e);
                    r.ssa = n->value ? "1" : "0";
                    r.type = VType::Bool;
                    return r;
                }
                case ExprKind::NoneLit:
                    r.ssa = "0"; r.type = VType::Int; return r;

                case ExprKind::StringLit: {
                    auto* n = static_cast<const StringLitExpr*>(e);
                    r.ssa = internString(n->value);
                    r.type = VType::Str;
                    return r;
                }

                case ExprKind::NameRef: {
                    auto* n = static_cast<const NameRefExpr*>(e);
                    const std::string& name = n->name;

                    std::string lookup = name;
                    auto fi = fromImports_.find(name);
                    if (fi != fromImports_.end()) {
                        lookup = mangle(fi->second) + "_" + name;
                    }
                    else if (!currentModulePrefix_.empty()) {
                        lookup = currentModulePrefix_ + name;
                    }

                    const std::string* slotPtr = nullptr;
                    {
                        auto sit = slots_.find(lookup);
                        if (sit != slots_.end()) slotPtr = &sit->second;
                    }
                    if (!slotPtr && lookup != name) {
                        auto sit2 = slots_.find(name);
                        if (sit2 != slots_.end()) slotPtr = &sit2->second;
                    }
                    if (!slotPtr) {
                        auto git = globalSlots_.find(name);
                        if (git != globalSlots_.end()) slotPtr = &git->second;
                    }
                    if (!slotPtr)
                        throw std::runtime_error(
                            "native: variable '" + name + "' not declared");
                    std::string t = newTemp();
                    line(t + " =l loadl " + *slotPtr);
                    r.ssa = t;

                    const VarInfo* viPtr = nullptr;
                    {
                        auto vi = varInfo_.find(lookup);
                        if (vi != varInfo_.end()) viPtr = &vi->second;
                    }
                    if (!viPtr) {
                        auto vi2 = varInfo_.find(name);
                        if (vi2 != varInfo_.end()) viPtr = &vi2->second;
                    }
                    if (!viPtr) {
                        auto gvi = globalVarInfo_.find(name);
                        if (gvi != globalVarInfo_.end()) viPtr = &gvi->second;
                    }
                    if (viPtr) {
                        r.type = viPtr->type;
                        r.cls = viPtr->clsName;
                        r.elemType = viPtr->elemType;
                        r.elemCls = viPtr->elemClsName;
                        r.valType = viPtr->valType;
                        r.tupleElemTypes = viPtr->tupleElemTypes;
                        r.tupleElemClsNames = viPtr->tupleElemClsNames;
                    }
                    return r;
                }

                case ExprKind::Grouping:
                    return emitExpr(static_cast<const GroupingExpr*>(e)->inner.get());

                case ExprKind::Unary: {
                    auto* n = static_cast<const UnaryExpr*>(e);

                    // Phase 15.4: &fn / &var / &arr[i] / &obj.f must run
                    // BEFORE we evaluate the operand as a value, because a
                    // bare function name is not a runtime variable.
                    if (n->op == UnOp::AddrOf) {
                        const Expr* op = n->operand.get();

                        // &topLevelFn → function pointer.
                        if (op->kind == ExprKind::NameRef) {
                            const auto* nm =
                                static_cast<const NameRefExpr*>(op);
                            std::string lookup = nm->name;
                            auto fi = fromImports_.find(lookup);
                            if (fi != fromImports_.end())
                                lookup = mangle(fi->second) + "_" + nm->name;
                            bool isVar = slots_.count(lookup) ||
                                slots_.count(nm->name) ||
                                globalSlots_.count(nm->name);
                            if (!isVar && topFnDecls_.count(nm->name)) {
                                std::string t = newTemp();
                                line(t + " =l copy $vayu_fn_" +
                                    mangle(nm->name));
                                r.ssa = t; r.type = VType::Ptr;
                                return r;
                            }
                        }

                        // &name → address of the slot.
                        if (op->kind == ExprKind::NameRef) {
                            const auto* nm =
                                static_cast<const NameRefExpr*>(op);
                            std::string lookup = nm->name;
                            auto fi = fromImports_.find(lookup);
                            if (fi != fromImports_.end())
                                lookup = mangle(fi->second) + "_" + nm->name;
                            else if (!currentModulePrefix_.empty())
                                lookup = currentModulePrefix_ + nm->name;
                            const std::string* slotPtr = nullptr;
                            {
                                auto s1 = slots_.find(lookup);
                                if (s1 != slots_.end()) slotPtr = &s1->second;
                            }
                            if (!slotPtr) {
                                auto s2 = slots_.find(nm->name);
                                if (s2 != slots_.end()) slotPtr = &s2->second;
                            }
                            if (!slotPtr) {
                                auto s3 = globalSlots_.find(nm->name);
                                if (s3 != globalSlots_.end())
                                    slotPtr = &s3->second;
                            }
                            if (!slotPtr)
                                throw std::runtime_error(
                                    "native: cannot take address of undefined "
                                    "name '" + nm->name + "'");
                            r.ssa = *slotPtr; r.type = VType::Ptr;
                            return r;
                        }

                        // &arr[i]
                        if (op->kind == ExprKind::Index) {
                            auto* ix = static_cast<const IndexExpr*>(op);
                            Val tgt = emitExpr(ix->target.get());
                            Val idx = emitExpr(ix->index.get());
                            std::string t = newTemp();
                            if (tgt.type == VType::List) {
                                line(t + " =l call $vayu_list_slot(l " +
                                    tgt.ssa + ", l " + idx.ssa + ")");
                            }
                            else if (tgt.type == VType::Map) {
                                line(t + " =l call $vayu_map_slot(l " +
                                    tgt.ssa + ", l " + idx.ssa + ")");
                            }
                            else {
                                throw std::runtime_error(
                                    "native: cannot take address of this index");
                            }
                            r.ssa = t; r.type = VType::Ptr;
                            return r;
                        }

                        // &obj.field
                        if (op->kind == ExprKind::Attr) {
                            auto* at = static_cast<const AttrExpr*>(op);
                            Val base = emitExpr(at->target.get());
                            if (base.type != VType::Obj)
                                throw std::runtime_error(
                                    "native: cannot take address of field on "
                                    "non-object");
                            const ClassInfo* ci = findClass(base.cls);
                            int fieldOff = -1;
                            if (ci) {
                                auto fit = ci->fieldOffsets.find(at->name);
                                if (fit != ci->fieldOffsets.end())
                                    fieldOff = fit->second;
                            }
                            if (fieldOff < 0) {
                                auto git = fieldGlobals_.find(at->name);
                                if (git != fieldGlobals_.end())
                                    fieldOff = git->second;
                            }
                            if (fieldOff < 0)
                                throw std::runtime_error(
                                    "native: '" + at->name +
                                    "' is not a field");
                            std::string addr = newTemp();
                            line(addr + " =l add " + base.ssa + ", " +
                                std::to_string(fieldOff));
                            r.ssa = addr; r.type = VType::Ptr;
                            return r;
                        }

                        // Fallback: heap cell.
                        Val v = emitExpr(op);
                        std::string cell = newTemp();
                        line(cell + " =l call $vayu_alloc(l 8)");
                        line("storel " + v.ssa + ", " + cell);
                        r.ssa = cell; r.type = VType::Ptr;
                        return r;
                    }

                    if (n->op == UnOp::Deref) {
                        Val v = emitExpr(n->operand.get());
                        std::string t = newTemp();
                        line(t + " =l loadl " + v.ssa);
                        r.ssa = t; r.type = VType::Int;
                        return r;
                    }

                    Val v = emitExpr(n->operand.get());
                    switch (n->op) {
                    case UnOp::Pos: return v;
                    case UnOp::Neg: {
                        if (v.type == VType::Float) {
                            std::string vd = newTemp();
                            line(vd + " =d cast " + v.ssa);
                            std::string nd = newTemp();
                            line(nd + " =d neg " + vd);
                            std::string rl = newTemp();
                            line(rl + " =l cast " + nd);
                            r.ssa = rl; r.type = VType::Float; return r;
                        }
                        std::string t = newTemp();
                        line(t + " =l sub 0, " + v.ssa);
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case UnOp::Not: {
                        std::string c = newTemp();
                        line(c + " =w ceql " + v.ssa + ", 0");
                        std::string ext = newTemp();
                        line(ext + " =l extsw " + c);
                        r.ssa = ext; r.type = VType::Bool; return r;
                    }
                    case UnOp::BNot: {
                        std::string t = newTemp();
                        line(t + " =l xor " + v.ssa + ", -1");
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    default: break;
                    }
                    return v;
                }

                case ExprKind::Binary: {
                    auto* n = static_cast<const BinaryExpr*>(e);

                    if (n->op == BinOp::And || n->op == BinOp::Or) {
                        /* Real short-circuit.  The old code emitted rhs
                           unconditionally, so a truthy lhs never protected
                           an unsafe rhs (e.g. `ok(x) or crash(x)`). */
                        bool isAnd = (n->op == BinOp::And);
                        Val a = emitExpr(n->lhs.get());
                        std::string aw = newTemp();
                        line(aw + " =w cnel " + a.ssa + ", 0");

                        std::string slot = newTemp();
                        line(slot + " =l alloc8 8");

                        std::string lShort = newLabel(isAnd ? "and_short_" : "or_short_");
                        std::string lFall = newLabel(isAnd ? "and_fall_" : "or_fall_");
                        std::string lEnd = newLabel(isAnd ? "and_end_" : "or_end_");

                        if (isAnd) line("jnz " + aw + ", " + lFall + ", " + lShort);
                        else       line("jnz " + aw + ", " + lShort + ", " + lFall);

                        raw(lShort);
                        line("storel " + std::string(isAnd ? "0" : "1") + ", " + slot);
                        line("jmp " + lEnd);

                        raw(lFall);
                        Val b = emitExpr(n->rhs.get());
                        std::string bw = newTemp();
                        line(bw + " =w cnel " + b.ssa + ", 0");
                        std::string ext = newTemp();
                        line(ext + " =l extsw " + bw);
                        line("storel " + ext + ", " + slot);

                        raw(lEnd);
                        std::string result = newTemp();
                        line(result + " =l loadl " + slot);
                        r.ssa = result; r.type = VType::Bool; return r;
                    }

                    Val a = emitExpr(n->lhs.get());
                    Val b = emitExpr(n->rhs.get());

                    bool strAdd = (n->op == BinOp::Add) &&
                        (a.type == VType::Str || b.type == VType::Str);
                    if (strAdd) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_concat(l " + a.ssa + ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return r;
                    }

                    bool strCmp = (n->op == BinOp::Eq || n->op == BinOp::NotEq) &&
                        (a.type == VType::Str || b.type == VType::Str);
                    if (strCmp) {
                        const char* fn = (n->op == BinOp::Eq) ? "$vayu_str_eq" : "$vayu_str_ne";
                        std::string t = newTemp();
                        line(t + " =l call " + fn + "(l " + a.ssa + ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return r;
                    }
                    bool tupleCmp =
                        (n->op == BinOp::Eq || n->op == BinOp::NotEq) &&
                        (a.type == VType::Tuple || b.type == VType::Tuple);
                    if (tupleCmp) {
                        const char* fn = (n->op == BinOp::Eq)
                            ? "$vayu_tuple_eq" : "$vayu_tuple_ne";
                        std::string t = newTemp();
                        line(t + " =l call " + std::string(fn) + "(l " +
                            a.ssa + ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return r;
                    }
                    if (n->op == BinOp::Add &&
                        a.type == VType::Tuple && b.type == VType::Tuple) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_tuple_concat(l " + a.ssa +
                            ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Tuple;
                        r.tupleElemTypes = a.tupleElemTypes;
                        for (auto& et : b.tupleElemTypes) r.tupleElemTypes.push_back(et);
                        r.tupleElemClsNames = a.tupleElemClsNames;
                        for (auto& ec : b.tupleElemClsNames) r.tupleElemClsNames.push_back(ec);
                        return r;
                    }
                    if (n->op == BinOp::Add && a.type == VType::Ptr) {
                        // p + n  →  p + n*8
                        std::string scaled = newTemp();
                        line(scaled + " =l mul " + b.ssa + ", 8");
                        std::string t = newTemp();
                        line(t + " =l add " + a.ssa + ", " + scaled);
                        r.ssa = t; r.type = VType::Ptr;
                        return r;
                    }

                    /* Phase 24.0 - float dispatch.  Floats cross the ABI as
                       bit patterns in i64 registers; arithmetic runs on `d`
                       and casts back.  `/` is always float (Vayu semantics),
                       even when both operands are int. */
                    bool anyF = (a.type == VType::Float) || (b.type == VType::Float);
                    bool isArithOrCmp =
                        n->op == BinOp::Add || n->op == BinOp::Sub ||
                        n->op == BinOp::Mul || n->op == BinOp::FloorDiv ||
                        n->op == BinOp::Mod || n->op == BinOp::Pow ||
                        n->op == BinOp::Eq || n->op == BinOp::NotEq ||
                        n->op == BinOp::Lt || n->op == BinOp::Gt ||
                        n->op == BinOp::LtEq || n->op == BinOp::GtEq;
                    bool useFloat = (n->op == BinOp::Div)
                        || (anyF && isArithOrCmp);
                    if (useFloat) {
                        std::string aD;
                        if (a.type == VType::Float) {
                            aD = newTemp();
                            line(aD + " =d cast " + a.ssa);
                        }
                        else {
                            aD = newTemp();
                            line(aD + " =d sltof " + a.ssa);
                        }
                        std::string bD;
                        if (b.type == VType::Float) {
                            bD = newTemp();
                            line(bD + " =d cast " + b.ssa);
                        }
                        else {
                            bD = newTemp();
                            line(bD + " =d sltof " + b.ssa);
                        }

                        if (n->op == BinOp::Add || n->op == BinOp::Sub ||
                            n->op == BinOp::Mul || n->op == BinOp::Div) {
                            const char* dop = "add";
                            if (n->op == BinOp::Sub) dop = "sub";
                            else if (n->op == BinOp::Mul) dop = "mul";
                            else if (n->op == BinOp::Div) dop = "div";
                            std::string t = newTemp();
                            line(t + " =d " + std::string(dop) + " " + aD + ", " + bD);
                            std::string rl = newTemp();
                            line(rl + " =l cast " + t);
                            r.ssa = rl; r.type = VType::Float; return r;
                        }
                        if (n->op == BinOp::FloorDiv) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_floordiv_d(l " + a.ssa +
                                ", l " + b.ssa + ")");
                            r.ssa = t; r.type = VType::Float; return r;
                        }
                        if (n->op == BinOp::Mod) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_mod_d(l " + a.ssa +
                                ", l " + b.ssa + ")");
                            r.ssa = t; r.type = VType::Float; return r;
                        }
                        if (n->op == BinOp::Pow) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_pow_d(l " + a.ssa +
                                ", l " + b.ssa + ")");
                            r.ssa = t; r.type = VType::Float; return r;
                        }
                        if (n->op == BinOp::Eq || n->op == BinOp::NotEq ||
                            n->op == BinOp::Lt || n->op == BinOp::Gt ||
                            n->op == BinOp::LtEq || n->op == BinOp::GtEq) {
                            /* QBE has no cneq for `d`.  NotEq is ceqd + xor 1. */
                            if (n->op == BinOp::NotEq) {
                                std::string cw = newTemp();
                                line(cw + " =w ceqd " + aD + ", " + bD);
                                std::string inv = newTemp();
                                line(inv + " =w xor " + cw + ", 1");
                                std::string ext = newTemp();
                                line(ext + " =l extsw " + inv);
                                r.ssa = ext; r.type = VType::Bool; return r;
                            }
                            const char* cop = "ceqd";
                            if (n->op == BinOp::Lt)   cop = "cltd";
                            else if (n->op == BinOp::Gt)   cop = "cgtd";
                            else if (n->op == BinOp::LtEq) cop = "cled";
                            else if (n->op == BinOp::GtEq) cop = "cged";
                            std::string cw = newTemp();
                            line(cw + " =w " + std::string(cop) + " " + aD + ", " + bD);
                            std::string ext = newTemp();
                            line(ext + " =l extsw " + cw);
                            r.ssa = ext; r.type = VType::Bool; return r;
                        }
                    }

                    switch (n->op) {
                    case BinOp::Add: {
                        std::string t = newTemp();
                        line(t + " =l add " + a.ssa + ", " + b.ssa);
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::Sub: {
                        std::string t = newTemp();
                        line(t + " =l sub " + a.ssa + ", " + b.ssa); r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::Mul: {
                        std::string t = newTemp();
                        line(t + " =l mul " + a.ssa + ", " + b.ssa); r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::Div: {
                        std::string t = newTemp();
                        line(t + " =l div " + a.ssa + ", " + b.ssa); r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::FloorDiv: {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_floordiv(l " + a.ssa + ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::Mod: {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_mod(l " + a.ssa + ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::Pow: {
                        if (n->rhs->kind != ExprKind::IntLit)
                            throw std::runtime_error(
                                "native: '**' requires a non-negative integer "
                                "literal exponent");
                        auto* lit = static_cast<const IntLitExpr*>(n->rhs.get());
                        if (lit->value < 0)
                            throw std::runtime_error(
                                "native: '**' with a negative exponent is not supported");
                        std::string t = newTemp();
                        line(t + " =l call $vayu_pow_int(l " + a.ssa +
                            ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Int; return r;
                    }

                    case BinOp::Eq: case BinOp::NotEq:
                    case BinOp::Lt: case BinOp::Gt:
                    case BinOp::LtEq: case BinOp::GtEq: {
                        const char* opName = nullptr;
                        switch (n->op) {
                        case BinOp::Eq:    opName = "ceql";  break;
                        case BinOp::NotEq: opName = "cnel";  break;
                        case BinOp::Lt:    opName = "csltl"; break;
                        case BinOp::Gt:    opName = "csgtl"; break;
                        case BinOp::LtEq:  opName = "cslel"; break;
                        case BinOp::GtEq:  opName = "csgel"; break;
                        default: break;
                        }
                        std::string c = newTemp();
                        line(c + " =w " + opName + " " + a.ssa + ", " + b.ssa);
                        std::string ext = newTemp();
                        line(ext + " =l extsw " + c);
                        r.ssa = ext; r.type = VType::Bool; return r;
                    }

                    case BinOp::BAnd: {
                        std::string t = newTemp();
                        line(t + " =l and " + a.ssa + ", " + b.ssa);
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::BOr: {
                        std::string t = newTemp();
                        line(t + " =l or " + a.ssa + ", " + b.ssa);
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::BXor: {
                        std::string t = newTemp();
                        line(t + " =l xor " + a.ssa + ", " + b.ssa);
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::Shl: {
                        std::string t = newTemp();
                        line(t + " =l shl " + a.ssa + ", " + b.ssa);
                        r.ssa = t; r.type = VType::Int; return r;
                    }
                    case BinOp::Shr: {
                        std::string t = newTemp();
                        line(t + " =l shr " + a.ssa + ", " + b.ssa);
                        r.ssa = t; r.type = VType::Int; return r;
                    }

                    case BinOp::In: {
                        if (b.type == VType::List) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_list_contains(l " + b.ssa +
                                ", l " + a.ssa + ")");
                            r.ssa = t; r.type = VType::Bool; return r;
                        }
                        if (b.type == VType::Map) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_map_has(l " + b.ssa +
                                ", l " + a.ssa + ")");
                            r.ssa = t; r.type = VType::Bool; return r;
                        }
                        if (b.type == VType::Str && a.type == VType::Str) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_str_contains(l " + b.ssa +
                                ", l " + a.ssa + ")");
                            r.ssa = t; r.type = VType::Bool; return r;
                        }
                        if (b.type == VType::Set) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_set_has(l " + b.ssa +
                                ", l " + a.ssa + ", l " +
                                std::to_string(tagOf(a.type)) + ")");
                            r.ssa = t; r.type = VType::Bool; return r;
                        }
                        if (b.type == VType::Tuple) {
                            std::string cnt = newTemp();
                            line(cnt + " =l call $vayu_tuple_count(l " + b.ssa +
                                ", l " + a.ssa + ", l " +
                                std::to_string(tagOf(a.type)) + ")");
                            std::string w = newTemp();
                            line(w + " =w cnel " + cnt + ", 0");
                            std::string ext = newTemp();
                            line(ext + " =l extsw " + w);
                            r.ssa = ext; r.type = VType::Bool; return r;
                        }
                        throw std::runtime_error("native: 'in' unsupported on these types");
                    }
                    case BinOp::Is:
                        throw std::runtime_error("native: 'is' not supported");
                    default: break;
                    }
                    throw std::runtime_error("native: unsupported binary op");
                }

                case ExprKind::Call:
                    return emitCall(static_cast<const CallExpr*>(e));

                case ExprKind::Index: {
                    auto* n = static_cast<const IndexExpr*>(e);
                    Val tgt = emitExpr(n->target.get());
                    Val idx = emitExpr(n->index.get());
                    if (tgt.type == VType::Ptr) {
                        // p[i]  →  loadl (p + i*8)
                        std::string scaled = newTemp();
                        line(scaled + " =l mul " + idx.ssa + ", 8");
                        std::string addr = newTemp();
                        line(addr + " =l add " + tgt.ssa + ", " + scaled);
                        std::string t = newTemp();
                        line(t + " =l loadl " + addr);
                        r.ssa = t; r.type = VType::Int;
                        return r;
                    }
                    if (tgt.type == VType::List) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_list_get(l " + tgt.ssa + ", l " + idx.ssa + ")");
                        r.ssa = t;
                        if (tgt.elemType == VType::Unknown) {
                            r.type = VType::Int;
                        }
                        else {
                            r.type = tgt.elemType;
                            r.cls = tgt.elemCls;
                            if (tgt.elemType == VType::Tuple) {
                                r.tupleElemTypes = tgt.tupleElemTypes;
                                r.tupleElemClsNames = tgt.tupleElemClsNames;
                            }
                        }
                        return r;
                    }
                    if (tgt.type == VType::Map) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_get(l " + tgt.ssa + ", l " + idx.ssa + ")");
                        r.ssa = t;
                        r.type = tgt.valType == VType::Unknown ? VType::Int : tgt.valType;
                        return r;
                    }
                    if (tgt.type == VType::Str) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_char_at(l " + tgt.ssa +
                            ", l " + idx.ssa + ")");
                        r.ssa = t; r.type = VType::Str;
                        return r;
                    }
                    if (tgt.type == VType::Tuple) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_tuple_get(l " + tgt.ssa +
                            ", l " + idx.ssa + ")");
                        r.ssa = t;

                        // Prefer element types carried on the Val itself
                        // (works for chained indexes and for loop vars).
                        if (n->index->kind == ExprKind::IntLit) {
                            long long i = static_cast<const IntLitExpr*>(
                                n->index.get())->value;
                            if (i >= 0 &&
                                (size_t)i < tgt.tupleElemTypes.size()) {
                                r.type = tgt.tupleElemTypes[(size_t)i];
                                r.cls = tgt.tupleElemClsNames[(size_t)i];
                                return r;
                            }
                        }
                        // Fall back to varInfo_ for a plain-name target.
                        if (n->target->kind == ExprKind::NameRef &&
                            n->index->kind == ExprKind::IntLit) {
                            const auto* nm = static_cast<const NameRefExpr*>(
                                n->target.get());
                            const auto* lit = static_cast<const IntLitExpr*>(
                                n->index.get());
                            const VarInfo* vi = nullptr;
                            auto it = varInfo_.find(nm->name);
                            if (it != varInfo_.end()) vi = &it->second;
                            if (!vi) {
                                auto g = globalVarInfo_.find(nm->name);
                                if (g != globalVarInfo_.end()) vi = &g->second;
                            }
                            if (vi && lit->value >= 0 &&
                                (size_t)lit->value < vi->tupleElemTypes.size()) {
                                r.type = vi->tupleElemTypes[(size_t)lit->value];
                                r.cls = vi->tupleElemClsNames[(size_t)lit->value];
                                return r;
                            }
                        }
                        r.type = VType::Int;
                        return r;
                    }
                    throw std::runtime_error("native: index on unsupported type");
                }

                case ExprKind::Attr: {
                    auto* n = static_cast<const AttrExpr*>(e);

                    if (n->target->kind == ExprKind::NameRef) {
                        const auto* tn = static_cast<const NameRefExpr*>(n->target.get());

                        // Phase 19.2 — gui event constants.
                        if (tn->name == "math") {
                            const std::string& mm = n->name;
                            double d = 0.0;
                            bool hasD = true;
                            /* The digits below are the exact decimal
                               expansions of pi, e, and tau.  A `double`
                               only holds ~15-17 significant decimal
                               digits, so everything past that is
                               silently truncated by the C++ compiler.
                               The long forms are kept for documentation
                               and for a future BigFloat type. */
                            if (mm == "pi")  d = 3.141592653589793238462643383279502884197169399375105820974944592307816406286208998628034825342117067;
                            else if (mm == "e")   d = 2.718281828459045235360287471352662497757247093699959574966967627724076630353547594571382178525166427;
                            else if (mm == "tau") d = 6.2831853071795864769252867665590057683943387987502116419498891846156328125724179972560696509622349006;
                            else if (mm == "inf") d = std::numeric_limits<double>::infinity();
                            else if (mm == "nan") d = std::numeric_limits<double>::quiet_NaN();
                            else hasD = false;
                            if (hasD) {
                                uint64_t bits = 0;
                                std::memcpy(&bits, &d, 8);
                                r.ssa = std::to_string((long long)bits);
                                r.type = VType::Float;
                                return r;
                            }
                        }
                        if (tn->name == "gui") {
                            const std::string& mm = n->name;
                            if (mm == "EV_NONE") { r.ssa = "0";  r.type = VType::Int; return r; }
                            if (mm == "EV_PAINT") { r.ssa = "1";  r.type = VType::Int; return r; }
                            if (mm == "EV_MOUSE_DOWN") { r.ssa = "2";  r.type = VType::Int; return r; }
                            if (mm == "EV_MOUSE_UP") { r.ssa = "3";  r.type = VType::Int; return r; }
                            if (mm == "EV_MOUSE_MOVE") { r.ssa = "4";  r.type = VType::Int; return r; }
                            if (mm == "EV_KEY_DOWN") { r.ssa = "5";  r.type = VType::Int; return r; }
                            if (mm == "EV_KEY_UP") { r.ssa = "6";  r.type = VType::Int; return r; }
                            if (mm == "EV_CLOSE") { r.ssa = "7";  r.type = VType::Int; return r; }
                            if (mm == "EV_TIMER") { r.ssa = "8";  r.type = VType::Int; return r; }
                            if (mm == "EV_MOUSE_WHEEL") { r.ssa = "9";  r.type = VType::Int; return r; }
                            if (mm == "EV_RESIZE") { r.ssa = "10"; r.type = VType::Int; return r; }
                            if (mm == "EV_FOCUS_IN") { r.ssa = "11"; r.type = VType::Int; return r; }
                            if (mm == "EV_FOCUS_OUT") { r.ssa = "12"; r.type = VType::Int; return r; }
                            if (mm == "EV_ENTER") { r.ssa = "13"; r.type = VType::Int; return r; }
                            if (mm == "EV_CONTROL") { r.ssa = "14"; r.type = VType::Int; return r; }
                            if (mm == "ALIGN_LEFT") { r.ssa = "0"; r.type = VType::Int; return r; }
                            if (mm == "ALIGN_CENTER") { r.ssa = "1"; r.type = VType::Int; return r; }
                            if (mm == "ALIGN_RIGHT") { r.ssa = "2"; r.type = VType::Int; return r; }
                        }

                        auto eit = enums_.find(tn->name);
                        if (eit != enums_.end()) {
                            auto iit = eit->second.find(n->name);
                            if (iit == eit->second.end())
                                throw std::runtime_error(
                                    "native: enum '" + tn->name +
                                    "' has no item '" + n->name + "'");
                            r.ssa = std::to_string(iit->second);
                            r.type = VType::Int;
                            return r;
                        }
                    }

                    if (n->target->kind == ExprKind::NameRef) {
                        const auto* tn = static_cast<const NameRefExpr*>(n->target.get());
                        auto sit = statics_.find(tn->name);
                        if (sit != statics_.end()) {
                            auto mit = sit->second.find(n->name);
                            if (mit != sit->second.end()) {
                                const std::string& slotName = mit->second;
                                const std::string* slotPtr = nullptr;
                                {
                                    auto git = slots_.find(slotName);
                                    if (git != slots_.end()) slotPtr = &git->second;
                                }
                                if (!slotPtr) {
                                    auto git2 = globalSlots_.find(slotName);
                                    if (git2 != globalSlots_.end()) slotPtr = &git2->second;
                                }
                                if (!slotPtr)
                                    throw std::runtime_error(
                                        "native: static '" + tn->name + "." +
                                        n->name + "' not initialized");
                                std::string t = newTemp();
                                line(t + " =l loadl " + *slotPtr);
                                r.ssa = t;
                                const VarInfo* viPtr = nullptr;
                                {
                                    auto vi = varInfo_.find(slotName);
                                    if (vi != varInfo_.end()) viPtr = &vi->second;
                                }
                                if (!viPtr) {
                                    auto gvi = globalVarInfo_.find(slotName);
                                    if (gvi != globalVarInfo_.end()) viPtr = &gvi->second;
                                }
                                if (viPtr) {
                                    r.type = viPtr->type;
                                    r.cls = viPtr->clsName;
                                    r.elemType = viPtr->elemType;
                                    r.elemCls = viPtr->elemClsName;
                                    r.valType = viPtr->valType;
                                }
                                else {
                                    r.type = VType::Int;
                                }
                                return r;
                            }
                        }
                    }

                    if (n->target->kind == ExprKind::NameRef) {
                        const auto* tn = static_cast<const NameRefExpr*>(n->target.get());
                        if (modules_.count(tn->name)) {
                            std::string prefixed = mangle(tn->name) + "_" + n->name;
                            auto sit = slots_.find(prefixed);
                            if (sit != slots_.end()) {
                                std::string t = newTemp();
                                line(t + " =l loadl " + sit->second);
                                r.ssa = t;
                                return r;
                            }
                            r.ssa = prefixed;
                            r.cls = "__fn__";
                            return r;
                        }
                    }

                    Val base = emitExpr(n->target.get());
                    if (base.type == VType::Exc) {
                        int off = (n->name == "message") ? 8 : 0;
                        std::string addr = newTemp();
                        line(addr + " =l add " + base.ssa + ", " + std::to_string(off));
                        std::string t = newTemp();
                        line(t + " =l loadl " + addr);
                        r.ssa = t;
                        r.type = VType::Str;
                        return r;
                    }
                    if (base.type != VType::Obj)
                        throw std::runtime_error(
                            "native: attribute access on non-object (type " +
                            std::to_string((int)base.type) + ") at line " +
                            std::to_string(e->loc.line));
                    const ClassInfo* ci = findClass(base.cls);
                    int fieldOff = -1;
                    if (ci) {
                        auto fit = ci->fieldOffsets.find(n->name);
                        if (fit != ci->fieldOffsets.end())
                            fieldOff = fit->second;
                    }
                    if (fieldOff < 0) {
                        auto git = fieldGlobals_.find(n->name);
                        if (git != fieldGlobals_.end()) fieldOff = git->second;
                    }
                    if (fieldOff < 0)
                        throw std::runtime_error(
                            "native: '" + n->name + "' is not a field of class '" +
                            base.cls + "'");
                    std::string addr = newTemp();
                    line(addr + " =l add " + base.ssa + ", " + std::to_string(fieldOff));
                    std::string t = newTemp();
                    line(t + " =l loadl " + addr);
                    r.ssa = t;
                    for (auto c = ci; c; c = c->parent) {
                        if (!c->decl) continue;
                        bool found = false;
                        for (auto& f : c->decl->fields) {
                            if (f.name == n->name) {
                                if (f.type) {
                                    inferFromAnnotation(f.type.get(), r);
                                }
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                    return r;
                }

                case ExprKind::ListLit: {
                    auto* n = static_cast<const ListLitExpr*>(e);
                    std::string lst = newTemp();
                    line(lst + " =l call $vayu_list_new()");
                    VType elemT = VType::Unknown;
                    std::string elemC;
                    std::vector<VType> elemTT;
                    std::vector<std::string> elemTC;
                    for (auto& el : n->elements) {
                        Val v = emitExpr(el.get());
                        if (elemT == VType::Unknown) {
                            elemT = v.type;
                            elemC = v.cls;
                            elemTT = v.tupleElemTypes;
                            elemTC = v.tupleElemClsNames;
                        }
                        line("call $vayu_list_push_tagged(l " + lst + ", l " + v.ssa +
                            ", l " + std::to_string(tagOf(v.type)) + ")");
                    }
                    r.ssa = lst; r.type = VType::List;
                    r.elemType = elemT; r.elemCls = elemC;
                    if (elemT == VType::Tuple) {
                        r.tupleElemTypes = elemTT;
                        r.tupleElemClsNames = elemTC;
                    }
                    return r;
                }

                case ExprKind::MapLit: {
                    auto* n = static_cast<const MapLitExpr*>(e);
                    std::string m = newTemp();
                    line(m + " =l call $vayu_map_new()");
                    VType valT = VType::Unknown;
                    for (auto& en : n->entries) {
                        Val k = emitExpr(en.key.get());
                        if (k.type != VType::Str)
                            throw std::runtime_error("native: map keys must be strings");
                        Val v = emitExpr(en.value.get());
                        if (valT == VType::Unknown) valT = v.type;
                        line("call $vayu_map_put(l " + m + ", l " + k.ssa +
                            ", l " + v.ssa + ")");
                    }
                    r.ssa = m; r.type = VType::Map; r.valType = valT;
                    return r;
                }
                case ExprKind::TupleLit: {
                    auto* n = static_cast<const TupleLitExpr*>(e);
                    std::string tup = newTemp();
                    line(tup + " =l call $vayu_tuple_new()");
                    for (auto& el : n->elements) {
                        Val v = emitExpr(el.get());
                        line("call $vayu_tuple_push_tagged(l " + tup + ", l " +
                            v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.tupleElemTypes.push_back(v.type);
                        r.tupleElemClsNames.push_back(v.cls);
                    }
                    r.ssa = tup; r.type = VType::Tuple;
                    return r;
                }
                case ExprKind::SetLit: {
                    auto* n = static_cast<const SetLitExpr*>(e);
                    std::string set = newTemp();
                    line(set + " =l call $vayu_set_new()");
                    for (auto& el : n->elements) {
                        Val v = emitExpr(el.get());
                        line("call $vayu_set_push_tagged(l " + set + ", l " +
                            v.ssa + ", l " + std::to_string(tagOf(v.type)) + ")");
                    }
                    r.ssa = set; r.type = VType::Set;
                    return r;
                }
                case ExprKind::Slice: {
                    auto* n = static_cast<const SliceExpr*>(e);
                    Val tgt = emitExpr(n->target.get());
                    bool hasStart = (n->start != nullptr);
                    bool hasEnd = (n->end != nullptr);
                    Val start; Val end;
                    if (hasStart) start = emitExpr(n->start.get());
                    else { start.ssa = "0"; start.type = VType::Int; }
                    if (hasEnd) end = emitExpr(n->end.get());
                    else { end.ssa = "0"; end.type = VType::Int; }

                    int64_t sliceTag = 0;
                    VType resultType = VType::Int;
                    if (tgt.type == VType::List) {
                        sliceTag = 1; resultType = VType::List;
                    }
                    else if (tgt.type == VType::Tuple) {
                        sliceTag = 5; resultType = VType::Tuple;
                    }
                    else if (tgt.type == VType::Str) {
                        sliceTag = 2; resultType = VType::Str;
                    }
                    else {
                        throw std::runtime_error(
                            "native: cannot slice value of type " +
                            std::to_string((int)tgt.type));
                    }
                    std::string t = newTemp();
                    line(t + " =l call $vayu_slice(l " + tgt.ssa +
                        ", l " + start.ssa + ", l " + end.ssa +
                        ", l " + (hasStart ? "1" : "0") +
                        ", l " + (hasEnd ? "1" : "0") +
                        ", l " + std::to_string(sliceTag) + ")");
                    r.ssa = t; r.type = resultType;
                    if (resultType == VType::List) {
                        r.elemType = tgt.elemType;
                        r.elemCls = tgt.elemCls;
                        r.tupleElemTypes = tgt.tupleElemTypes;
                        r.tupleElemClsNames = tgt.tupleElemClsNames;
                    }
                    else if (resultType == VType::Tuple) {
                        r.tupleElemTypes = tgt.tupleElemTypes;
                        r.tupleElemClsNames = tgt.tupleElemClsNames;
                    }
                    return r;
                }
                case ExprKind::FloatLit: {
                    auto* n = static_cast<const FloatLitExpr*>(e);
                    double d = n->value;
                    uint64_t bits = 0;
                    std::memcpy(&bits, &d, 8);
                    r.ssa = std::to_string((long long)bits);
                    r.type = VType::Float;
                    return r;
                }
                case ExprKind::CharLit:
                    throw std::runtime_error("native: char literals not yet supported");
                case ExprKind::Lambda:
                    throw std::runtime_error("native: lambdas not yet supported");
                case ExprKind::GenericType:
                    throw std::runtime_error("native: generic type used as value");
                }
                return r;
            }

            VType typeOfAnnotation(const Expr* e) {
                if (!e) return VType::Unknown;
                if (e->kind == ExprKind::NameRef) {
                    const auto* n = static_cast<const NameRefExpr*>(e);
                    const std::string& s = n->name;
                    if (s == "int")   return VType::Int;
                    if (s == "bool")  return VType::Bool;
                    if (s == "str")   return VType::Str;
                    if (s == "float") return VType::Float;
                    if (s == "list")  return VType::List;
                    if (s == "map")   return VType::Map;
                    if (classes_.count(s)) return VType::Obj;
                }
                if (e->kind == ExprKind::GenericType) {
                    const auto* g = static_cast<const GenericTypeExpr*>(e);
                    if (g->name == "list") return VType::List;
                    if (g->name == "map")  return VType::Map;
                }
                return VType::Unknown;
            }

            std::string classNameOfAnnotation(const Expr* e) {
                if (e && e->kind == ExprKind::NameRef) {
                    const auto* n = static_cast<const NameRefExpr*>(e);
                    if (classes_.count(n->name)) return n->name;
                }
                return {};
            }

            bool tryPureBuiltin(const std::string& name, const CallExpr* n,
                Val& r) {
                if (name == "hash") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: hash() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    if (v.type == VType::Str)
                        line(t + " =l call $vayu_hash_str(l " + v.ssa + ")");
                    else
                        line(t + " =l call $vayu_hash_int(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return true;
                }
                if (name == "id") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: id() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    r = v;
                    r.type = VType::Int;
                    return true;
                }
                if (name == "callable") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: callable() takes 1 argument");
                    bool isCallable = false;
                    if (n->args[0].value->kind == ExprKind::NameRef) {
                        const std::string& argName =
                            static_cast<const NameRefExpr*>(
                                n->args[0].value.get())->name;
                        if (topFnDecls_.count(argName) ||
                            classes_.count(argName) ||
                            generatorFunctions_.count(argName))
                            isCallable = true;
                    }
                    r.ssa = isCallable ? "1" : "0";
                    r.type = VType::Bool;
                    return true;
                }
                if (name == "repr") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: repr() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Str) { r = v; return true; }
                    if (v.type == VType::Int || v.type == VType::Bool) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_int_to_str(l " + v.ssa + ", l " +
                            std::to_string(kindOf(v.type)) + ")");
                        r.ssa = t; r.type = VType::Str;
                        return true;
                    }
                    r = v;
                    return true;
                }
                if (name == "round") {
                    if (n->args.empty() || n->args.size() > 2)
                        throw std::runtime_error(
                            "native: round() takes 1 or 2 arguments");
                    Val v = emitExpr(n->args[0].value.get());
                    if (n->args.size() == 2)
                        throw std::runtime_error(
                            "native: round(x, n) requires floats (not supported)");
                    std::string t = newTemp();
                    line(t + " =l call $vayu_round_int(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return true;
                }
                if (name == "pow") {
                    if (n->args.size() < 2 || n->args.size() > 3)
                        throw std::runtime_error(
                            "native: pow() takes 2 or 3 arguments");
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    if (n->args.size() == 3) {
                        Val m = emitExpr(n->args[2].value.get());
                        std::string t = newTemp();
                        line(t + " =l call $vayu_pow_mod(l " + a.ssa +
                            ", l " + b.ssa + ", l " + m.ssa + ")");
                        r.ssa = t; r.type = VType::Int;
                        return true;
                    }
                    std::string t = newTemp();
                    line(t + " =l call $vayu_pow_int(l " + a.ssa +
                        ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return true;
                }
                if (name == "divmod") {
                    if (n->args.size() != 2)
                        throw std::runtime_error(
                            "native: divmod() takes 2 arguments");
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    std::string lst = newTemp();
                    line(lst + " =l call $vayu_list_new()");
                    std::string q = newTemp();
                    line(q + " =l call $vayu_floordiv(l " + a.ssa +
                        ", l " + b.ssa + ")");
                    std::string m = newTemp();
                    line(m + " =l call $vayu_mod(l " + a.ssa +
                        ", l " + b.ssa + ")");
                    line("call $vayu_list_push(l " + lst + ", l " + q + ")");
                    line("call $vayu_list_push(l " + lst + ", l " + m + ")");
                    r.ssa = lst; r.type = VType::List; r.elemType = VType::Int;
                    return true;
                }
                if (name == "sign") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: sign() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_sign_int(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return true;
                }
                if (name == "gcd") {
                    if (n->args.size() != 2)
                        throw std::runtime_error("native: gcd() takes 2 arguments");
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_gcd(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return true;
                }
                if (name == "lcm") {
                    if (n->args.size() != 2)
                        throw std::runtime_error("native: lcm() takes 2 arguments");
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_lcm(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return true;
                }
                if (name == "clamp") {
                    if (n->args.size() != 3)
                        throw std::runtime_error("native: clamp() takes 3 arguments");
                    Val x = emitExpr(n->args[0].value.get());
                    Val lo = emitExpr(n->args[1].value.get());
                    Val hi = emitExpr(n->args[2].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_clamp_int(l " + x.ssa +
                        ", l " + lo.ssa + ", l " + hi.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return true;
                }
                if (name == "enumerate") {
                    if (n->args.size() != 1)
                        throw std::runtime_error(
                            "native: enumerate() takes 1 argument");
                    Val src = emitExpr(n->args[0].value.get());
                    if (src.type != VType::List)
                        throw std::runtime_error(
                            "native: enumerate() requires a list");
                    std::string out = newTemp();
                    line(out + " =l call $vayu_list_new()");
                    std::string idx = "%eidx_" + std::to_string(nextLabel_++);
                    line(idx + " =l alloc8 8");
                    line("storel 0, " + idx);
                    std::string lenT = newTemp();
                    line(lenT + " =l call $vayu_list_len(l " + src.ssa + ")");
                    std::string lLoop = newLabel("enum_loop_");
                    std::string lEnd = newLabel("enum_end_");
                    raw(lLoop);
                    {
                        std::string i = newTemp();
                        line(i + " =l loadl " + idx);
                        std::string c = newTemp();
                        line(c + " =w csltl " + i + ", " + lenT);
                        line("jnz " + c + ", @enum_body_" +
                            std::to_string(nextLabel_) + ", " + lEnd);
                        raw("@enum_body_" + std::to_string(nextLabel_++));
                        std::string elem = newTemp();
                        line(elem + " =l call $vayu_list_get(l " + src.ssa +
                            ", l " + i + ")");
                        std::string pair = newTemp();
                        line(pair + " =l call $vayu_tuple_new()");
                        line("call $vayu_tuple_push_tagged(l " + pair + ", l " + i + ", l 0)");
                        line("call $vayu_tuple_push_tagged(l " + pair + ", l " + elem +
                            ", l " + std::to_string(tagOf(src.elemType)) + ")");
                        line("call $vayu_list_push_tagged(l " + out + ", l " + pair + ", l 5)");
                        std::string nx = newTemp();
                        line(nx + " =l add " + i + ", 1");
                        line("storel " + nx + ", " + idx);
                        line("jmp " + lLoop);
                    }
                    raw(lEnd);
                    r.ssa = out; r.type = VType::List;
                    r.elemType = VType::Tuple;
                    r.tupleElemTypes = { VType::Int, src.elemType };
                    r.tupleElemClsNames = { "", src.elemCls };
                    return true;
                }
                if (name == "zip") {
                    if (n->args.size() != 2)
                        throw std::runtime_error("native: zip() takes 2 arguments");
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    if (a.type != VType::List || b.type != VType::List)
                        throw std::runtime_error(
                            "native: zip() requires two lists");
                    std::string out = newTemp();
                    line(out + " =l call $vayu_list_new()");
                    std::string idx = "%zidx_" + std::to_string(nextLabel_++);
                    line(idx + " =l alloc8 8");
                    line("storel 0, " + idx);
                    std::string la = newTemp();
                    line(la + " =l call $vayu_list_len(l " + a.ssa + ")");
                    std::string lb = newTemp();
                    line(lb + " =l call $vayu_list_len(l " + b.ssa + ")");
                    std::string lmin = newTemp();
                    line(lmin + " =w csltl " + la + ", " + lb);
                    std::string cext = newTemp();
                    line(cext + " =l extsw " + lmin);
                    std::string notc = newTemp();
                    line(notc + " =l xor " + cext + ", 1");
                    std::string pa = newTemp();
                    line(pa + " =l mul " + la + ", " + cext);
                    std::string pb = newTemp();
                    line(pb + " =l mul " + lb + ", " + notc);
                    std::string nlen = newTemp();
                    line(nlen + " =l add " + pa + ", " + pb);
                    std::string lLoop = newLabel("zip_loop_");
                    std::string lEnd = newLabel("zip_end_");
                    raw(lLoop);
                    {
                        std::string i = newTemp();
                        line(i + " =l loadl " + idx);
                        std::string c = newTemp();
                        line(c + " =w csltl " + i + ", " + nlen);
                        std::string lBody = newLabel("zip_body_");
                        line("jnz " + c + ", " + lBody + ", " + lEnd);
                        raw(lBody);
                        std::string ea = newTemp();
                        line(ea + " =l call $vayu_list_get(l " + a.ssa +
                            ", l " + i + ")");
                        std::string eb = newTemp();
                        line(eb + " =l call $vayu_list_get(l " + b.ssa +
                            ", l " + i + ")");
                        std::string pair = newTemp();
                        line(pair + " =l call $vayu_tuple_new()");
                        line("call $vayu_tuple_push_tagged(l " + pair + ", l " + ea +
                            ", l " + std::to_string(tagOf(a.elemType)) + ")");
                        line("call $vayu_tuple_push_tagged(l " + pair + ", l " + eb +
                            ", l " + std::to_string(tagOf(b.elemType)) + ")");
                        line("call $vayu_list_push_tagged(l " + out + ", l " + pair + ", l 5)");
                        std::string nx = newTemp();
                        line(nx + " =l add " + i + ", 1");
                        line("storel " + nx + ", " + idx);
                        line("jmp " + lLoop);
                    }
                    raw(lEnd);
                    r.ssa = out; r.type = VType::List;
                    r.elemType = VType::Tuple;
                    r.tupleElemTypes = { a.elemType, b.elemType };
                    r.tupleElemClsNames = { a.elemCls, b.elemCls };
                    return true;
                }
                if (name == "reversed") {
                    if (n->args.size() != 1)
                        throw std::runtime_error(
                            "native: reversed() takes 1 argument");
                    Val src = emitExpr(n->args[0].value.get());
                    if (src.type != VType::List)
                        throw std::runtime_error(
                            "native: reversed() requires a list");
                    std::string out = newTemp();
                    line(out + " =l call $vayu_list_new()");
                    std::string i = "%ridx_" + std::to_string(nextLabel_++);
                    line(i + " =l alloc8 8");
                    std::string lenT = newTemp();
                    line(lenT + " =l call $vayu_list_len(l " + src.ssa + ")");
                    std::string minus1 = newTemp();
                    line(minus1 + " =l sub " + lenT + ", 1");
                    line("storel " + minus1 + ", " + i);
                    std::string lLoop = newLabel("rev_loop_");
                    std::string lEnd = newLabel("rev_end_");
                    std::string lBody = newLabel("rev_body_");
                    line("jmp " + lLoop);
                    raw(lLoop);
                    {
                        std::string iv = newTemp();
                        line(iv + " =l loadl " + i);
                        std::string c = newTemp();
                        line(c + " =w csgel " + iv + ", 0");
                        line("jnz " + c + ", " + lBody + ", " + lEnd);
                    }
                    raw(lBody);
                    {
                        std::string iv = newTemp();
                        line(iv + " =l loadl " + i);
                        std::string e = newTemp();
                        line(e + " =l call $vayu_list_get(l " + src.ssa +
                            ", l " + iv + ")");
                        line("call $vayu_list_push_tagged(l " + out + ", l " + e +
                            ", l " + std::to_string(tagOf(src.elemType)) + ")");
                        std::string nx = newTemp();
                        line(nx + " =l sub " + iv + ", 1");
                        line("storel " + nx + ", " + i);
                        line("jmp " + lLoop);
                    }
                    raw(lEnd);
                    r.ssa = out; r.type = VType::List;
                    r.elemType = src.elemType;
                    return true;
                }

                if (name == "isinstance") {
                    if (n->args.size() != 2)
                        throw std::runtime_error(
                            "native: isinstance() takes 2 arguments");
                    Val v = emitExpr(n->args[0].value.get());
                    const Expr* texpr = n->args[1].value.get();
                    if (texpr->kind != ExprKind::NameRef)
                        throw std::runtime_error(
                            "native: isinstance() type argument must be a "
                            "class name or primitive-type name");
                    const std::string& tn =
                        static_cast<const NameRefExpr*>(texpr)->name;

                    bool ok = false;
                    if (tn == "int")        ok = (v.type == VType::Int);
                    else if (tn == "str")   ok = (v.type == VType::Str);
                    else if (tn == "bool")  ok = (v.type == VType::Bool);
                    else if (tn == "list")  ok = (v.type == VType::List);
                    else if (tn == "map")   ok = (v.type == VType::Map);
                    else if (tn == "None")  ok = (v.type == VType::Int &&
                        v.ssa == "0");
                    else if (v.type == VType::Obj) {
                        for (auto c = findClass(v.cls); c; c = c->parent)
                            if (c->name == tn) { ok = true; break; }
                    }
                    else if (v.type == VType::Exc) {
                        std::string typeLbl = internString(tn);
                        std::string et = newTemp();
                        line(et + " =l call $vayu_get_exc_type()");
                        (void)typeLbl;
                        std::string cond = newTemp();
                        line(cond + " =w call $vayu_str_eq(l " + et +
                            ", l " + typeLbl + ")");
                        std::string ext = newTemp();
                        line(ext + " =l extsw " + cond);
                        r.ssa = ext; r.type = VType::Bool;
                        return true;
                    }

                    r.ssa = ok ? "1" : "0"; r.type = VType::Bool;
                    return true;
                }
                if (name == "issubclass") {
                    if (n->args.size() != 2)
                        throw std::runtime_error(
                            "native: issubclass() takes 2 arguments");
                    const Expr* ae = n->args[0].value.get();
                    const Expr* be = n->args[1].value.get();
                    if (ae->kind != ExprKind::NameRef ||
                        be->kind != ExprKind::NameRef)
                        throw std::runtime_error(
                            "native: issubclass() arguments must be class names");
                    const std::string& an =
                        static_cast<const NameRefExpr*>(ae)->name;
                    const std::string& bn =
                        static_cast<const NameRefExpr*>(be)->name;
                    bool ok = false;
                    const ClassInfo* a = findClass(an);
                    for (auto c = a; c; c = c->parent)
                        if (c->name == bn) { ok = true; break; }
                    r.ssa = ok ? "1" : "0"; r.type = VType::Bool;
                    return true;
                }
                if (name == "hasattr") {
                    if (n->args.size() != 2 ||
                        n->args[1].value->kind != ExprKind::StringLit)
                        throw std::runtime_error(
                            "native: hasattr(obj, \"name\") requires a string literal");
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type != VType::Obj) {
                        r.ssa = "0"; r.type = VType::Bool;
                        return true;
                    }
                    const std::string& attrName =
                        static_cast<const StringLitExpr*>(
                            n->args[1].value.get())->value;
                    bool ok = false;
                    const ClassInfo* ci = findClass(v.cls);
                    for (auto c = ci; c && !ok; c = c->parent) {
                        if (c->fieldOffsets.count(attrName)) { ok = true; break; }
                        if (c->methods.count(attrName)) { ok = true; break; }
                    }
                    r.ssa = ok ? "1" : "0"; r.type = VType::Bool;
                    return true;
                }
                if (name == "getattr") {
                    if (n->args.size() != 2 ||
                        n->args[1].value->kind != ExprKind::StringLit)
                        throw std::runtime_error(
                            "native: getattr(obj, \"name\") requires a string literal");
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type != VType::Obj)
                        throw std::runtime_error(
                            "native: getattr() receiver must be an object");
                    const std::string& attrName =
                        static_cast<const StringLitExpr*>(
                            n->args[1].value.get())->value;
                    const ClassInfo* ci = findClass(v.cls);
                    int off = -1;
                    for (auto c = ci; c && off < 0; c = c->parent) {
                        auto fit = c->fieldOffsets.find(attrName);
                        if (fit != c->fieldOffsets.end()) off = fit->second;
                    }
                    if (off < 0)
                        throw std::runtime_error(
                            "native: getattr: class '" + v.cls +
                            "' has no field '" + attrName + "'");
                    std::string addr = newTemp();
                    line(addr + " =l add " + v.ssa + ", " + std::to_string(off));
                    std::string t = newTemp();
                    line(t + " =l loadl " + addr);
                    r.ssa = t;
                    for (auto c = ci; c; c = c->parent) {
                        bool found = false;
                        for (auto& f : c->decl->fields) {
                            if (f.name == attrName) {
                                if (f.type) inferFromAnnotation(f.type.get(), r);
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                    return true;
                }
                if (name == "dir") {
                    if (n->args.size() != 1)
                        throw std::runtime_error(
                            "native: dir() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    std::vector<std::string> names;
                    if (v.type == VType::Obj) {
                        const ClassInfo* ci = findClass(v.cls);
                        for (auto c = ci; c; c = c->parent) {
                            for (auto& f : c->fieldOffsets) names.push_back(f.first);
                            for (auto& m : c->methods) names.push_back(m.first);
                        }
                    }
                    std::sort(names.begin(), names.end());
                    names.erase(std::unique(names.begin(), names.end()),
                        names.end());
                    std::string lst = newTemp();
                    line(lst + " =l call $vayu_list_new()");
                    for (auto& nm : names) {
                        std::string lbl = internString(nm);
                        line("call $vayu_list_push(l " + lst + ", l " + lbl + ")");
                    }
                    r.ssa = lst; r.type = VType::List;
                    r.elemType = VType::Str;
                    return true;
                }

                if (name == "setattr") {
                    if (n->args.size() != 3 ||
                        n->args[1].value->kind != ExprKind::StringLit)
                        throw std::runtime_error(
                            "native: setattr(obj, \"field\", value) requires "
                            "a string literal field name");
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type != VType::Obj)
                        throw std::runtime_error(
                            "native: setattr() receiver must be an object");
                    const std::string& attrName =
                        static_cast<const StringLitExpr*>(
                            n->args[1].value.get())->value;
                    Val val = emitExpr(n->args[2].value.get());
                    const ClassInfo* ci = findClass(v.cls);
                    int off = -1;
                    for (auto c = ci; c && off < 0; c = c->parent) {
                        auto fit = c->fieldOffsets.find(attrName);
                        if (fit != c->fieldOffsets.end()) off = fit->second;
                    }
                    if (off < 0)
                        throw std::runtime_error(
                            "native: setattr: class '" + v.cls +
                            "' has no field '" + attrName + "'");
                    std::string addr = newTemp();
                    line(addr + " =l add " + v.ssa + ", " + std::to_string(off));
                    line("storel " + val.ssa + ", " + addr);
                    r.ssa = "0"; r.type = VType::Void;
                    return true;
                }

                if (name == "delattr") {
                    throw std::runtime_error(
                        "native: delattr() is not supported (object layout "
                        "is fixed at construction)");
                }
                if (name == "comb") {
                    if (n->args.size() != 2)
                        throw std::runtime_error("native: comb() takes 2 arguments");
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_comb(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return true;
                }
                if (name == "perm") {
                    if (n->args.size() != 2)
                        throw std::runtime_error("native: perm() takes 2 arguments");
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_perm(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return true;
                }
                if (name == "isqrt") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: isqrt() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_isqrt(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return true;
                }
                if (name == "factorial") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: factorial() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_factorial(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return true;
                }
                if (name == "list") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: list() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::List) { r = v; return true; }
                    if (v.type == VType::Tuple) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_tuple_from_list(l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::List;
                        r.elemType = VType::Unknown;
                        return true;
                    }
                    if (v.type == VType::Str) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_split(l " + v.ssa +
                            ", l " + internString("") + ")");
                        r.ssa = t; r.type = VType::List; r.elemType = VType::Str;
                        return true;
                    }
                    throw std::runtime_error("native: list() unsupported arg");
                }
                if (name == "tuple") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: tuple() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Tuple) { r = v; return true; }
                    if (v.type == VType::List) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_tuple_from_list(l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::Tuple; return true;
                    }
                    if (v.type == VType::Str) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_tuple_from_str(l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::Tuple; return true;
                    }
                    throw std::runtime_error("native: tuple() unsupported arg");
                }
                if (name == "set") {
                    if (n->args.size() != 1)
                        throw std::runtime_error("native: set() takes 1 argument");
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Set) { r = v; return true; }
                    if (v.type == VType::List) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_set_from_list(l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::Set; return true;
                    }
                    throw std::runtime_error("native: set() unsupported arg");
                }
                return false;
            }

            bool tryBuiltinMethod(const std::string& recvName, const Val& recv,
                const CallExpr* call, Val& r) {
                auto argV = [&](size_t i) { return emitExpr(call->args[i].value.get()); };

                if (recv.type == VType::List) {
                    if (recvName == "append") {
                        Val v = argV(0);
                        line("call $vayu_list_push_tagged(l " + recv.ssa +
                            ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "pop") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_list_pop(l " + recv.ssa + ")");
                        r.ssa = t; r.type = recv.elemType; return true;
                    }
                    if (recvName == "clear") {
                        line("call $vayu_list_clear(l " + recv.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "contains") {
                        Val v = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_list_contains(l " + recv.ssa +
                            ", l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "insert") {
                        Val i = argV(0), v = argV(1);
                        line("call $vayu_list_insert_tagged(l " + recv.ssa +
                            ", l " + i.ssa + ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "remove") {
                        Val v = argV(0);
                        line("call $vayu_list_remove(l " + recv.ssa + ", l " + v.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "extend") {
                        Val v = argV(0);
                        line("call $vayu_list_extend(l " + recv.ssa +
                            ", l " + v.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "count") {
                        Val v = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_list_count(l " + recv.ssa +
                            ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = t; r.type = VType::Int; return true;
                    }
                    if (recvName == "reverse") {
                        line("call $vayu_list_reverse(l " + recv.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "sort") {
                        line("call $vayu_list_sort(l " + recv.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "copy") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_list_copy(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::List;
                        r.elemType = recv.elemType;
                        r.elemCls = recv.elemCls;
                        return true;
                    }
                    if (recvName == "first") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_list_first(l " + recv.ssa + ")");
                        r.ssa = t; r.type = recv.elemType;
                        r.cls = recv.elemCls; return true;
                    }
                    if (recvName == "last") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_list_last(l " + recv.ssa + ")");
                        r.ssa = t; r.type = recv.elemType;
                        r.cls = recv.elemCls; return true;
                    }
                }
                if (recv.type == VType::Map) {
                    if (recvName == "put") {
                        Val k = argV(0), v = argV(1);
                        line("call $vayu_map_put(l " + recv.ssa + ", l " + k.ssa +
                            ", l " + v.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "get") {
                        Val k = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_get(l " + recv.ssa + ", l " + k.ssa + ")");
                        r.ssa = t; r.type = recv.valType; return true;
                    }
                    if (recvName == "contains") {
                        Val k = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_has(l " + recv.ssa + ", l " + k.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "remove") {
                        Val k = argV(0);
                        line("call $vayu_map_remove(l " + recv.ssa + ", l " + k.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "clear") {
                        line("call $vayu_map_clear(l " + recv.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "keys") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_keys(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::List; r.elemType = VType::Str; return true;
                    }
                    if (recvName == "set") {
                        Val k = argV(0), v = argV(1);
                        line("call $vayu_map_put(l " + recv.ssa + ", l " +
                            k.ssa + ", l " + v.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "get_or") {
                        Val k = argV(0), d = argV(1);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_get_or(l " + recv.ssa +
                            ", l " + k.ssa + ", l " + d.ssa + ")");
                        r.ssa = t; r.type = recv.valType; return true;
                    }
                    if (recvName == "pop") {
                        Val k = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_pop(l " + recv.ssa +
                            ", l " + k.ssa + ")");
                        r.ssa = t; r.type = recv.valType; return true;
                    }
                    if (recvName == "pop_or") {
                        Val k = argV(0), d = argV(1);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_pop_or(l " + recv.ssa +
                            ", l " + k.ssa + ", l " + d.ssa + ")");
                        r.ssa = t; r.type = recv.valType; return true;
                    }
                    if (recvName == "update") {
                        Val o = argV(0);
                        line("call $vayu_map_update(l " + recv.ssa +
                            ", l " + o.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "copy") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_copy(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Map;
                        r.elemType = recv.elemType;
                        r.elemCls = recv.elemCls;
                        r.valType = recv.valType;
                        return true;
                    }
                    if (recvName == "has_key") {
                        Val k = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_has(l " + recv.ssa +
                            ", l " + k.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "values") {
                        int64_t vt = tagOf(recv.valType);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_values(l " + recv.ssa +
                            ", l " + std::to_string(vt) + ")");
                        r.ssa = t; r.type = VType::List;
                        r.elemType = recv.valType;
                        return true;
                    }
                    if (recvName == "items") {
                        int64_t vt = tagOf(recv.valType);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_map_items(l " + recv.ssa +
                            ", l " + std::to_string(vt) + ")");
                        r.ssa = t; r.type = VType::List;
                        r.elemType = VType::List;
                        return true;
                    }
                }
                if (recv.type == VType::Str) {
                    if (recvName == "upper" || recvName == "lower") {
                        const char* fn = (recvName == "upper") ? "$vayu_str_upper" : "$vayu_str_lower";
                        std::string t = newTemp();
                        line(t + " =l call " + fn + "(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "contains") {
                        Val sub = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_contains(l " + recv.ssa +
                            ", l " + sub.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "find") {
                        Val sub = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_find(l " + recv.ssa +
                            ", l " + sub.ssa + ")");
                        r.ssa = t; r.type = VType::Int; return true;
                    }
                    if (recvName == "starts_with" || recvName == "ends_with") {
                        Val sub = argV(0);
                        const char* fn = (recvName == "starts_with")
                            ? "$vayu_str_starts_with" : "$vayu_str_ends_with";
                        std::string t = newTemp();
                        line(t + " =l call " + fn + "(l " + recv.ssa + ", l " + sub.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "char_at") {
                        Val i = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_char_at(l " + recv.ssa +
                            ", l " + i.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "split") {
                        std::string t = newTemp();
                        if (call->args.empty()) {
                            line(t + " =l call $vayu_str_split_ws(l " + recv.ssa + ")");
                        }
                        else {
                            Val sep = argV(0);
                            line(t + " =l call $vayu_str_split(l " + recv.ssa +
                                ", l " + sep.ssa + ")");
                        }
                        r.ssa = t; r.type = VType::List; r.elemType = VType::Str;
                        return true;
                    }
                    if (recvName == "replace") {
                        Val o = argV(0), nw = argV(1);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_replace(l " + recv.ssa +
                            ", l " + o.ssa + ", l " + nw.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "substr") {
                        Val a = argV(0), b = argV(1);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_substr(l " + recv.ssa +
                            ", l " + a.ssa + ", l " + b.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "strip") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_strip(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "lstrip") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_lstrip(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "rstrip") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_rstrip(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "is_digit") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_is_digit(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "is_alpha") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_is_alpha(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "is_space") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_is_space(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "to_int") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_to_int(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Int; return true;
                    }
                    if (recvName == "join") {
                        Val lst = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_join(l " + recv.ssa +
                            ", l " + lst.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "capitalize") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_capitalize(l " +
                            recv.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "title") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_title(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "swapcase") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_swapcase(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "center" || recvName == "ljust" ||
                        recvName == "rjust") {
                        Val w = argV(0);
                        Val fill;
                        if (call->args.size() >= 2) {
                            fill = argV(1);
                        }
                        else {
                            fill.ssa = internString(" ");
                            fill.type = VType::Str;
                        }
                        const char* fn = (recvName == "center")
                            ? "$vayu_str_center"
                            : (recvName == "ljust")
                            ? "$vayu_str_ljust"
                            : "$vayu_str_rjust";
                        std::string t = newTemp();
                        line(t + " =l call " + std::string(fn) + "(l " +
                            recv.ssa + ", l " + w.ssa + ", l " + fill.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "zfill") {
                        Val w = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_zfill(l " + recv.ssa +
                            ", l " + w.ssa + ")");
                        r.ssa = t; r.type = VType::Str; return true;
                    }
                    if (recvName == "count" || recvName == "rfind") {
                        Val sub = argV(0);
                        const char* fn = (recvName == "count")
                            ? "$vayu_str_count" : "$vayu_str_rfind";
                        std::string t = newTemp();
                        line(t + " =l call " + std::string(fn) + "(l " +
                            recv.ssa + ", l " + sub.ssa + ")");
                        r.ssa = t; r.type = VType::Int; return true;
                    }
                    if (recvName == "partition" || recvName == "rpartition") {
                        Val sep = argV(0);
                        int64_t right = (recvName == "rpartition") ? 1 : 0;
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_partition(l " + recv.ssa +
                            ", l " + sep.ssa + ", l " +
                            std::to_string(right) + ")");
                        r.ssa = t; r.type = VType::List;
                        r.elemType = VType::Str;
                        return true;
                    }
                    if (recvName == "splitlines") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_splitlines(l " +
                            recv.ssa + ")");
                        r.ssa = t; r.type = VType::List;
                        r.elemType = VType::Str;
                        return true;
                    }
                    if (recvName == "rsplit") {
                        Val sep; Val ms;
                        if (call->args.size() >= 1) sep = argV(0);
                        else { sep.ssa = internString(""); sep.type = VType::Str; }
                        if (call->args.size() >= 2) ms = argV(1);
                        else { ms.ssa = "-1"; ms.type = VType::Int; }
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_rsplit(l " + recv.ssa +
                            ", l " + sep.ssa + ", l " + ms.ssa + ")");
                        r.ssa = t; r.type = VType::List;
                        r.elemType = VType::Str;
                        return true;
                    }
                    if (recvName == "is_lower" || recvName == "is_upper" ||
                        recvName == "is_alnum" || recvName == "is_ascii") {
                        const char* fn =
                            (recvName == "is_lower") ? "$vayu_str_is_lower" :
                            (recvName == "is_upper") ? "$vayu_str_is_upper" :
                            (recvName == "is_alnum") ? "$vayu_str_is_alnum" :
                            "$vayu_str_is_ascii";
                        std::string t = newTemp();
                        line(t + " =l call " + std::string(fn) + "(l " +
                            recv.ssa + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                }
                if (recv.type == VType::Tuple) {
                    if (recvName == "count") {
                        Val v = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_tuple_count(l " + recv.ssa +
                            ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = t; r.type = VType::Int; return true;
                    }
                    if (recvName == "index") {
                        Val v = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_tuple_index(l " + recv.ssa +
                            ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = t; r.type = VType::Int; return true;
                    }
                }
                if (recv.type == VType::Set) {
                    if (recvName == "add") {
                        Val v = argV(0);
                        line("call $vayu_set_push_tagged(l " + recv.ssa +
                            ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "contains") {
                        Val v = argV(0);
                        std::string t = newTemp();
                        line(t + " =l call $vayu_set_has(l " + recv.ssa +
                            ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = t; r.type = VType::Bool; return true;
                    }
                    if (recvName == "remove" || recvName == "discard") {
                        Val v = argV(0);
                        line("call $vayu_set_remove(l " + recv.ssa +
                            ", l " + v.ssa + ", l " +
                            std::to_string(tagOf(v.type)) + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "clear") {
                        line("call $vayu_set_clear(l " + recv.ssa + ")");
                        r.ssa = "0"; r.type = VType::Void; return true;
                    }
                    if (recvName == "copy") {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_set_copy(l " + recv.ssa + ")");
                        r.ssa = t; r.type = VType::Set; return true;
                    }
                    if (recvName == "union" ||
                        recvName == "intersection" ||
                        recvName == "difference") {
                        Val other = argV(0);
                        const char* fn =
                            (recvName == "union") ? "$vayu_set_union" :
                            (recvName == "intersection") ? "$vayu_set_intersection" :
                            "$vayu_set_difference";
                        std::string t = newTemp();
                        line(t + " =l call " + std::string(fn) + "(l " +
                            recv.ssa + ", l " + other.ssa + ")");
                        r.ssa = t; r.type = VType::Set;
                        return true;
                    }
                }
                return false;
            }

            Val emitFsCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "read_dir") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_read_dir(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::List; r.elemType = VType::Str;
                    return r;
                }
                if (m == "mkdir") {
                    Val p = a0();
                    line("call $vayu_fs_mkdir(l " + p.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "rmdir") {
                    Val p = a0();
                    line("call $vayu_fs_rmdir(l " + p.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "remove") {
                    Val p = a0();
                    line("call $vayu_fs_remove(l " + p.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "rename") {
                    Val a = a0();
                    Val b = a1();
                    line("call $vayu_fs_rename(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "exists") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_exists(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }
                if (m == "is_file") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_is_file(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }
                if (m == "is_dir") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_is_dir(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }
                if (m == "size") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_size(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "cwd") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_cwd()");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "abs") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_abs(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "join") {
                    Val a = a0();
                    Val b = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_join(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "extension") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_extension(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "basename") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_basename(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "dirname") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_fs_dirname(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                throw std::runtime_error("native: fs has no method '" + m + "'");
            }
            Val emitTimeCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;

                if (m == "now") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_time_now()");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "now_ms") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_time_now_ms()");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "sleep") {
                    Val ms = emitExpr(n->args[0].value.get());
                    line("call $vayu_time_sleep(l " + ms.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "format") {
                    Val u = emitExpr(n->args[0].value.get());
                    Val f = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_time_format(l " + u.ssa +
                        ", l " + f.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                throw std::runtime_error("native: time has no method '" + m + "'");
            }

            Val emitJsonCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "parse") {
                    Val s = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_parse(l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Json";
                    return r;
                }
                if (m == "stringify") {
                    Val v = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_stringify(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "get") {
                    Val v = a0(); Val k = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_get(l " + v.ssa + ", l " + k.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Json";
                    return r;
                }
                if (m == "index") {
                    Val v = a0(); Val i = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_index(l " + v.ssa + ", l " + i.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Json";
                    return r;
                }
                if (m == "as_int") {
                    Val v = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_as_int(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "as_str") {
                    Val v = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_as_str(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "as_bool") {
                    Val v = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_as_bool(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }
                if (m == "len") {
                    Val v = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_len(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "has") {
                    Val v = a0(); Val k = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_has(l " + v.ssa + ", l " + k.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }
                if (m == "type") {
                    Val v = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_type(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "is_null" || m == "is_int" || m == "is_str" ||
                    m == "is_bool" || m == "is_list" || m == "is_map") {
                    Val v = a0();
                    std::string fn;
                    if (m == "is_null") fn = "$vayu_json_is_null";
                    else if (m == "is_int")  fn = "$vayu_json_is_int";
                    else if (m == "is_str")  fn = "$vayu_json_is_str";
                    else if (m == "is_bool") fn = "$vayu_json_is_bool";
                    else if (m == "is_list") fn = "$vayu_json_is_list";
                    else                     fn = "$vayu_json_is_map";
                    std::string t = newTemp();
                    line(t + " =l call " + fn + "(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }
                if (m == "keys") {
                    Val v = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_keys(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::List; r.elemType = VType::Str;
                    return r;
                }
                if (m == "make_null") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_make_null()");
                    r.ssa = t; r.type = VType::Int; r.cls = "Json";
                    return r;
                }
                if (m == "make_int") {
                    Val x = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_make_int(l " + x.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Json";
                    return r;
                }
                if (m == "make_str") {
                    Val x = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_make_str(l " + x.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Json";
                    return r;
                }
                if (m == "make_bool") {
                    Val x = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_json_make_bool(l " + x.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Json";
                    return r;
                }
                throw std::runtime_error("native: json has no method '" + m + "'");
            }
            Val emitRegexCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "match") {
                    Val p = a0(); Val s = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_regex_match(l " + p.ssa + ", l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }
                if (m == "search") {
                    Val p = a0(); Val s = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_regex_search(l " + p.ssa + ", l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "find_all") {
                    Val p = a0(); Val s = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_regex_find_all(l " + p.ssa + ", l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::List; r.elemType = VType::Str;
                    return r;
                }
                if (m == "replace") {
                    Val p = a0(); Val s = a1();
                    Val repl = emitExpr(n->args[2].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_regex_replace(l " + p.ssa + ", l " + s.ssa +
                        ", l " + repl.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "fields") {
                    Val p = a0(); Val s = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_regex_split(l " + p.ssa + ", l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::List; r.elemType = VType::Str;
                    return r;
                }
                throw std::runtime_error("native: regex has no method '" + m + "'");
            }
            Val emitThreadCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;

                if (m == "spawn") {
                    if (n->args.size() != 2)
                        throw std::runtime_error("native: thread.spawn expects (name, arg)");
                    if (n->args[0].value->kind != ExprKind::StringLit)
                        throw std::runtime_error(
                            "native: thread.spawn first argument must be a string literal");
                    const auto* sl = static_cast<const StringLitExpr*>(
                        n->args[0].value.get());
                    const std::string& fnName = sl->value;
                    auto fit = topFnDecls_.find(fnName);
                    if (fit == topFnDecls_.end())
                        throw std::runtime_error(
                            "native: thread.spawn: no function named '" + fnName + "'");
                    if (fit->second->params.size() != 1)
                        throw std::runtime_error(
                            "native: thread.spawn: '" + fnName +
                            "' must take exactly one parameter");
                    Val arg = emitExpr(n->args[1].value.get());
                    std::string sym = "$vayu_fn_" + mangle(fnName);
                    std::string t = newTemp();
                    line(t + " =l call $vayu_thread_spawn(l " + sym +
                        ", l " + arg.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Thread";
                    return r;
                }
                if (m == "join") {
                    Val h = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_thread_join(l " + h.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "id") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_thread_id()");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "mutex_new") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_mutex_new()");
                    r.ssa = t; r.type = VType::Int; r.cls = "Mutex";
                    return r;
                }
                if (m == "lock") {
                    Val h = emitExpr(n->args[0].value.get());
                    line("call $vayu_mutex_lock(l " + h.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "unlock") {
                    Val h = emitExpr(n->args[0].value.get());
                    line("call $vayu_mutex_unlock(l " + h.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                throw std::runtime_error("native: thread has no method '" + m + "'");
            }

            Val emitNetCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "listen") {
                    Val p = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_listen(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Server";
                    return r;
                }
                if (m == "accept") {
                    Val s = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_accept(l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Conn";
                    return r;
                }
                if (m == "connect") {
                    Val h = a0(); Val p = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_connect(l " + h.ssa + ", l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Int; r.cls = "Conn";
                    return r;
                }
                if (m == "send") {
                    Val h = a0(); Val d = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_send(l " + h.ssa + ", l " + d.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "recv") {
                    Val h = a0(); Val nn = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_recv(l " + h.ssa + ", l " + nn.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "send_line") {
                    Val h = a0(); Val d = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_send_line(l " + h.ssa + ", l " + d.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "recv_line") {
                    Val h = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_recv_line(l " + h.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "close") {
                    Val h = a0();
                    line("call $vayu_net_close(l " + h.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "server_port") {
                    Val h = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_net_server_port(l " + h.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                throw std::runtime_error("native: net has no method '" + m + "'");
            }
            Val emitCryptoCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "sha256") {
                    Val s = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_crypto_sha256(l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "md5") {
                    Val s = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_crypto_md5(l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "hmac_sha256") {
                    Val k = a0(); Val msg = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_crypto_hmac_sha256(l " + k.ssa +
                        ", l " + msg.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "base64_encode") {
                    Val s = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_crypto_base64_encode(l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "base64_decode") {
                    Val s = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_crypto_base64_decode(l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "random_bytes") {
                    Val nn = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_crypto_random_bytes(l " + nn.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                throw std::runtime_error("native: crypto has no method '" + m + "'");
            }

            Val emitRandomCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "seed") {
                    Val s = a0();
                    line("call $vayu_random_seed(l " + s.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "randint") {
                    Val lo = a0(); Val hi = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_random_randint(l " + lo.ssa +
                        ", l " + hi.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "randrange") {
                    Val lo = a0(); Val hi = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_random_randrange(l " + lo.ssa +
                        ", l " + hi.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (m == "choice") {
                    Val l = a0();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_random_choice(l " + l.ssa + ")");
                    r.ssa = t; r.type = VType::Unknown;
                    return r;
                }
                if (m == "shuffle") {
                    Val l = a0();
                    line("call $vayu_random_shuffle(l " + l.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "sample") {
                    Val l = a0(); Val k = a1();
                    std::string t = newTemp();
                    line(t + " =l call $vayu_random_sample(l " + l.ssa +
                        ", l " + k.ssa + ")");
                    r.ssa = t; r.type = VType::List; r.elemType = VType::Unknown;
                    return r;
                }
                throw std::runtime_error("native: random has no method '" + m + "'");
            }

            Val emitOsCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;

                if (m == "getenv") {
                    Val s = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_os_getenv(l " + s.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "setenv") {
                    Val k = emitExpr(n->args[0].value.get());
                    Val v = emitExpr(n->args[1].value.get());
                    line("call $vayu_os_setenv(l " + k.ssa + ", l " + v.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "platform") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_os_platform()");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "hostname") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_os_hostname()");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "cwd") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_os_cwd()");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (m == "chdir") {
                    Val p = emitExpr(n->args[0].value.get());
                    line("call $vayu_os_chdir(l " + p.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                if (m == "exit") {
                    Val c = emitExpr(n->args[0].value.get());
                    line("call $vayu_os_exit(l " + c.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }
                throw std::runtime_error("native: os has no method '" + m + "'");
            }

            Val emitGuiCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };
                auto a2 = [&]() { return emitExpr(n->args[2].value.get()); };
                auto a3 = [&]() { return emitExpr(n->args[3].value.get()); };
                auto a4 = [&]() { return emitExpr(n->args[4].value.get()); };
                auto a5 = [&]() { return emitExpr(n->args[5].value.get()); };

                if (m == "init") { std::string t = newTemp(); line(t + " =l call $vayu_gui_init()"); r.ssa = t; r.type = VType::Bool; return r; }
                if (m == "invalidate") { line("call $vayu_gui_invalidate()"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "create_window") { Val tt = a0(); Val w = a1(); Val h = a2(); std::string t = newTemp(); line(t + " =l call $vayu_gui_create_window(l " + tt.ssa + ", l " + w.ssa + ", l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; r.cls = "Window"; return r; }
                if (m == "show_window") { line("call $vayu_gui_show_window(l " + a0().ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "set_title") { Val h = a0(); Val tt = a1(); line("call $vayu_gui_set_title(l " + h.ssa + ", l " + tt.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "set_size") { Val h = a0(); Val w = a1(); Val k = a2(); line("call $vayu_gui_set_size(l " + h.ssa + ", l " + w.ssa + ", l " + k.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "set_callback") { Val fp = a0(); line("call $vayu_gui_set_callback(l " + fp.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "set_timer") { Val ms = a0(); line("call $vayu_gui_set_timer(l " + ms.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "clear_timer") { line("call $vayu_gui_clear_timer()"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mouse_x") { std::string t = newTemp(); line(t + " =l call $vayu_gui_mouse_x()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "mouse_y") { std::string t = newTemp(); line(t + " =l call $vayu_gui_mouse_y()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "last_key") { std::string t = newTemp(); line(t + " =l call $vayu_gui_last_key()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "wheel_delta") { std::string t = newTemp(); line(t + " =l call $vayu_gui_wheel_delta()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "window_w") { std::string t = newTemp(); line(t + " =l call $vayu_gui_window_w()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "window_h") { std::string t = newTemp(); line(t + " =l call $vayu_gui_window_h()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "run") { line("call $vayu_gui_run(l " + a0().ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "close") { line("call $vayu_gui_close(l " + a0().ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "quit") { line("call $vayu_gui_quit()"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "clear") { Val c = a0(); line("call $vayu_gui_clear(l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fill_rect") { Val x = a0(); Val y = a1(); Val w = a2(); Val h = a3(); Val c = a4(); line("call $vayu_gui_fill_rect(l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + h.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "outline_rect") { Val x = a0(); Val y = a1(); Val w = a2(); Val h = a3(); Val c = a4(); line("call $vayu_gui_outline_rect(l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + h.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "line") { Val x1 = a0(); Val y1 = a1(); Val x2 = a2(); Val y2 = a3(); Val c = a4(); line("call $vayu_gui_line(l " + x1.ssa + ", l " + y1.ssa + ", l " + x2.ssa + ", l " + y2.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "text") { Val s = a0(); Val x = a1(); Val y = a2(); Val c = a3(); line("call $vayu_gui_text(l " + s.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "rect_hit") { Val x = a0(); Val y = a1(); Val w = a2(); Val h = a3(); Val px = a4(); Val py = a5(); std::string t = newTemp(); line(t + " =l call $vayu_gui_rect_hit(l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + h.ssa + ", l " + px.ssa + ", l " + py.ssa + ")"); r.ssa = t; r.type = VType::Bool; return r; }

                if (m == "canvas_new") { Val w = a0(); Val h = a1(); std::string t = newTemp(); line(t + " =l call $vayu_gui_canvas_new(l " + w.ssa + ", l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "canvas_from_window") { std::string t = newTemp(); line(t + " =l call $vayu_gui_canvas_from_window()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "canvas_free") { Val h = a0(); line("call $vayu_gui_canvas_free(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "canvas_clear") { Val h = a0(); Val c = a1(); line("call $vayu_gui_canvas_clear(l " + h.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "canvas_fill_rect") { Val h = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val k = a4(); Val c = a5(); line("call $vayu_gui_canvas_fill_rect(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + k.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "canvas_outline_rect") { Val h = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val k = a4(); Val c = a5(); Val t = emitExpr(n->args[6].value.get()); line("call $vayu_gui_canvas_outline_rect(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + k.ssa + ", l " + c.ssa + ", l " + t.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "blit_canvas") { Val s = a0(); Val x = a1(); Val y = a2(); line("call $vayu_gui_blit_canvas(l " + s.ssa + ", l " + x.ssa + ", l " + y.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "circle") { Val h = a0(); Val cx = a1(); Val cy = a2(); Val rr = a3(); Val c = a4(); line("call $vayu_gui_circle(l " + h.ssa + ", l " + cx.ssa + ", l " + cy.ssa + ", l " + rr.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "circle_outline") { Val h = a0(); Val cx = a1(); Val cy = a2(); Val rr = a3(); Val c = a4(); line("call $vayu_gui_circle_outline(l " + h.ssa + ", l " + cx.ssa + ", l " + cy.ssa + ", l " + rr.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "ellipse") { Val h = a0(); Val cx = a1(); Val cy = a2(); Val rx = a3(); Val ry = a4(); Val c = a5(); line("call $vayu_gui_ellipse(l " + h.ssa + ", l " + cx.ssa + ", l " + cy.ssa + ", l " + rx.ssa + ", l " + ry.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "ellipse_outline") { Val h = a0(); Val cx = a1(); Val cy = a2(); Val rx = a3(); Val ry = a4(); Val c = a5(); line("call $vayu_gui_ellipse_outline(l " + h.ssa + ", l " + cx.ssa + ", l " + cy.ssa + ", l " + rx.ssa + ", l " + ry.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "rounded_rect") { Val h = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val k = a4(); Val rr = a5(); Val c = emitExpr(n->args[6].value.get()); line("call $vayu_gui_rounded_rect(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + k.ssa + ", l " + rr.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "rounded_rect_outline") { Val h = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val k = a4(); Val rr = a5(); Val c = emitExpr(n->args[6].value.get()); line("call $vayu_gui_rounded_rect_outline(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + k.ssa + ", l " + rr.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "arc") { Val h = a0(); Val cx = a1(); Val cy = a2(); Val rr = a3(); Val sd = a4(); Val sw = a5(); Val c = emitExpr(n->args[6].value.get()); line("call $vayu_gui_arc(l " + h.ssa + ", l " + cx.ssa + ", l " + cy.ssa + ", l " + rr.ssa + ", l " + sd.ssa + ", l " + sw.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "polygon") { Val h = a0(); Val pts = a1(); Val c = a2(); line("call $vayu_gui_polygon(l " + h.ssa + ", l " + pts.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "polygon_outline") { Val h = a0(); Val pts = a1(); Val c = a2(); line("call $vayu_gui_polygon_outline(l " + h.ssa + ", l " + pts.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "line_width") { Val h = a0(); Val p = a1(); line("call $vayu_gui_line_width(l " + h.ssa + ", l " + p.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "line_cap") { Val h = a0(); Val p = a1(); line("call $vayu_gui_line_cap(l " + h.ssa + ", l " + p.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "line_join") { Val h = a0(); Val p = a1(); line("call $vayu_gui_line_join(l " + h.ssa + ", l " + p.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "font_new") { Val nm = a0(); Val sz = a1(); Val wt = a2(); Val it = a3(); std::string t = newTemp(); line(t + " =l call $vayu_gui_font_new(l " + nm.ssa + ", l " + sz.ssa + ", l " + wt.ssa + ", l " + it.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "font_free") { Val h = a0(); line("call $vayu_gui_font_free(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "font_default") { std::string t = newTemp(); line(t + " =l call $vayu_gui_font_default()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "text_ex") { Val c = a0(); Val f = a1(); Val s = a2(); Val x = a3(); Val y = a4(); Val a = a5(); line("call $vayu_gui_text_ex(l " + c.ssa + ", l " + f.ssa + ", l " + s.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + a.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "create_input") { Val p = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val h = a4(); std::string t = newTemp(); line(t + " =l call $vayu_gui_create_input(l " + p.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; r.cls = "Input"; return r; }
                if (m == "input_get") { Val e = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_input_get(l " + e.ssa + ")"); r.ssa = t; r.type = VType::Str; return r; }
                if (m == "input_set") { Val e = a0(); Val s = a1(); line("call $vayu_gui_input_set(l " + e.ssa + ", l " + s.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "input_focus") { Val e = a0(); line("call $vayu_gui_input_focus(l " + e.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "create_checkbox") { Val p = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val h = a4(); Val l = a5(); std::string t = newTemp(); line(t + " =l call $vayu_gui_create_checkbox(l " + p.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + h.ssa + ", l " + l.ssa + ")"); r.ssa = t; r.type = VType::Int; r.cls = "Checkbox"; return r; }
                if (m == "checkbox_get") { Val e = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_checkbox_get(l " + e.ssa + ")"); r.ssa = t; r.type = VType::Bool; return r; }
                if (m == "checkbox_set") { Val e = a0(); Val v = a1(); line("call $vayu_gui_checkbox_set(l " + e.ssa + ", l " + v.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "create_slider") { Val p = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val h = a4(); std::string t = newTemp(); line(t + " =l call $vayu_gui_create_slider(l " + p.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; r.cls = "Slider"; return r; }
                if (m == "slider_get") { Val e = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_slider_get(l " + e.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "slider_set") { Val e = a0(); Val v = a1(); line("call $vayu_gui_slider_set(l " + e.ssa + ", l " + v.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "slider_set_range") { Val e = a0(); Val lo = a1(); Val hi = a2(); line("call $vayu_gui_slider_set_range(l " + e.ssa + ", l " + lo.ssa + ", l " + hi.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "create_listbox") { Val p = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val h = a4(); std::string t = newTemp(); line(t + " =l call $vayu_gui_create_listbox(l " + p.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; r.cls = "ListBox"; return r; }
                if (m == "listbox_add") { Val e = a0(); Val s = a1(); line("call $vayu_gui_listbox_add(l " + e.ssa + ", l " + s.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "listbox_count") { Val e = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_listbox_count(l " + e.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "listbox_index") { Val e = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_listbox_index(l " + e.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "listbox_get") { Val e = a0(); Val i = a1(); std::string t = newTemp(); line(t + " =l call $vayu_gui_listbox_get(l " + e.ssa + ", l " + i.ssa + ")"); r.ssa = t; r.type = VType::Str; return r; }
                if (m == "listbox_clear") { Val e = a0(); line("call $vayu_gui_listbox_clear(l " + e.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "bitmap_load") { Val p = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_bitmap_load(l " + p.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "bitmap_free") { Val h = a0(); line("call $vayu_gui_bitmap_free(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "bitmap_width") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_bitmap_width(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "bitmap_height") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_bitmap_height(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "draw_bitmap") { Val c = a0(); Val b = a1(); Val x = a2(); Val y = a3(); line("call $vayu_gui_draw_bitmap(l " + c.ssa + ", l " + b.ssa + ", l " + x.ssa + ", l " + y.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "draw_bitmap_scaled") { Val c = a0(); Val b = a1(); Val x = a2(); Val y = a3(); Val w = a4(); Val k = a5(); line("call $vayu_gui_draw_bitmap_scaled(l " + c.ssa + ", l " + b.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + k.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "draw_bitmap_part") { Val c = a0(); Val b = a1(); Val sx = a2(); Val sy = a3(); Val sw = a4(); Val sh = a5(); Val dx = emitExpr(n->args[6].value.get()); Val dy = emitExpr(n->args[7].value.get()); line("call $vayu_gui_draw_bitmap_part(l " + c.ssa + ", l " + b.ssa + ", l " + sx.ssa + ", l " + sy.ssa + ", l " + sw.ssa + ", l " + sh.ssa + ", l " + dx.ssa + ", l " + dy.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "draw_bitmap_alpha") { Val c = a0(); Val b = a1(); Val x = a2(); Val y = a3(); Val al = a4(); line("call $vayu_gui_draw_bitmap_alpha(l " + c.ssa + ", l " + b.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + al.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "canvas_save_png") { Val c = a0(); Val p = a1(); std::string t = newTemp(); line(t + " =l call $vayu_gui_canvas_save_png(l " + c.ssa + ", l " + p.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "bitmap_from_pixels") { Val w = a0(); Val h = a1(); Val lst = a2(); std::string t = newTemp(); line(t + " =l call $vayu_gui_bitmap_from_pixels(l " + w.ssa + ", l " + h.ssa + ", l " + lst.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }

                if (m == "push_transform") { Val h = a0(); line("call $vayu_gui_push_transform(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "pop_transform") { Val h = a0(); line("call $vayu_gui_pop_transform(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "reset_transform") { Val h = a0(); line("call $vayu_gui_reset_transform(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "translate") { Val h = a0(); Val dx = a1(); Val dy = a2(); line("call $vayu_gui_translate(l " + h.ssa + ", l " + dx.ssa + ", l " + dy.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "rotate") { Val h = a0(); Val d = a1(); line("call $vayu_gui_rotate(l " + h.ssa + ", l " + d.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "rotate_at") { Val h = a0(); Val d = a1(); Val cx = a2(); Val cy = a3(); line("call $vayu_gui_rotate_at(l " + h.ssa + ", l " + d.ssa + ", l " + cx.ssa + ", l " + cy.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "scale") { Val h = a0(); Val sx = a1(); Val sy = a2(); line("call $vayu_gui_scale(l " + h.ssa + ", l " + sx.ssa + ", l " + sy.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "clip_rect") { Val h = a0(); Val x = a1(); Val y = a2(); Val w = a3(); Val k = a4(); line("call $vayu_gui_clip_rect(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + w.ssa + ", l " + k.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "clip_reset") { Val h = a0(); line("call $vayu_gui_clip_reset(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fill_mode") { Val h = a0(); Val mm = a1(); line("call $vayu_gui_fill_mode(l " + h.ssa + ", l " + mm.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "compositing_mode") { Val h = a0(); Val mm = a1(); line("call $vayu_gui_compositing_mode(l " + h.ssa + ", l " + mm.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "text_align") { Val h = a0(); Val mm = a1(); line("call $vayu_gui_text_align(l " + h.ssa + ", l " + mm.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "text_width") { Val h = a0(); Val f = a1(); Val s = a2(); std::string t = newTemp(); line(t + " =l call $vayu_gui_text_width(l " + h.ssa + ", l " + f.ssa + ", l " + s.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "text_height") { Val h = a0(); Val f = a1(); Val s = a2(); std::string t = newTemp(); line(t + " =l call $vayu_gui_text_height(l " + h.ssa + ", l " + f.ssa + ", l " + s.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "font_height") { Val f = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_font_height(l " + f.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "font_line_spacing") { Val f = a0(); std::string t = newTemp(); line(t + " =l call $vayu_gui_font_line_spacing(l " + f.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }

                if (m == "listbox_set_index") { Val e = a0(); Val i = a1(); line("call $vayu_gui_listbox_set_index(l " + e.ssa + ", l " + i.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                throw std::runtime_error("native: gui has no method '" + m + "'");
            }

            Val emitRasterCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };
                auto a2 = [&]() { return emitExpr(n->args[2].value.get()); };
                auto a3 = [&]() { return emitExpr(n->args[3].value.get()); };
                auto a4 = [&]() { return emitExpr(n->args[4].value.get()); };
                auto a5 = [&]() { return emitExpr(n->args[5].value.get()); };
                auto a6 = [&]() { return emitExpr(n->args[6].value.get()); };

                if (m == "fb_new") { Val w = a0(); Val h = a1(); std::string t = newTemp(); line(t + " =l call $vayu_raster_fb_new(l " + w.ssa + ", l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "fb_free") { Val h = a0(); line("call $vayu_raster_fb_free(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fb_width") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_raster_fb_width(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "fb_height") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_raster_fb_height(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "fb_clear") { Val h = a0(); Val c = a1(); line("call $vayu_raster_fb_clear(l " + h.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fb_set") { Val h = a0(); Val x = a1(); Val y = a2(); Val c = a3(); line("call $vayu_raster_fb_set(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fb_get") { Val h = a0(); Val x = a1(); Val y = a2(); std::string t = newTemp(); line(t + " =l call $vayu_raster_fb_get(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "fb_present") { Val h = a0(); Val c = a1(); Val x = a2(); Val y = a3(); line("call $vayu_raster_fb_present(l " + h.ssa + ", l " + c.ssa + ", l " + x.ssa + ", l " + y.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "draw_line") { Val h = a0(); Val x0 = a1(); Val y0 = a2(); Val x1 = a3(); Val y1 = a4(); Val c = a5(); line("call $vayu_raster_draw_line(l " + h.ssa + ", l " + x0.ssa + ", l " + y0.ssa + ", l " + x1.ssa + ", l " + y1.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "draw_tri") { Val h = a0(); Val x0 = a1(); Val y0 = a2(); Val x1 = a3(); Val y1 = a4(); Val x2 = a5(); Val y2 = a6(); Val c = emitExpr(n->args[7].value.get()); line("call $vayu_raster_draw_tri(l " + h.ssa + ", l " + x0.ssa + ", l " + y0.ssa + ", l " + x1.ssa + ", l " + y1.ssa + ", l " + x2.ssa + ", l " + y2.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "fb_enable_depth") { Val h = a0(); line("call $vayu_raster_fb_enable_depth(l " + h.ssa + ")");  r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fb_disable_depth") { Val h = a0(); line("call $vayu_raster_fb_disable_depth(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fb_clear_depth") { Val h = a0(); line("call $vayu_raster_fb_clear_depth(l " + h.ssa + ")");   r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "mat_new") { std::string t = newTemp(); line(t + " =l call $vayu_raster_mat_new()");      r.ssa = t; r.type = VType::Int; return r; }
                if (m == "mat_free") { Val h = a0(); line("call $vayu_raster_mat_free(l " + h.ssa + ")");          r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_identity") { Val h = a0(); line("call $vayu_raster_mat_identity(l " + h.ssa + ")");      r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_mul") { Val d = a0(); Val a = a1(); Val b = a2(); line("call $vayu_raster_mat_mul(l " + d.ssa + ", l " + a.ssa + ", l " + b.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_translate") { Val h = a0(); Val x = a1(); Val y = a2(); Val z = a3(); line("call $vayu_raster_mat_translate(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + z.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_rotate_x") { Val h = a0(); Val d = a1(); line("call $vayu_raster_mat_rotate_x(l " + h.ssa + ", l " + d.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_rotate_y") { Val h = a0(); Val d = a1(); line("call $vayu_raster_mat_rotate_y(l " + h.ssa + ", l " + d.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_rotate_z") { Val h = a0(); Val d = a1(); line("call $vayu_raster_mat_rotate_z(l " + h.ssa + ", l " + d.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_perspective") { Val h = a0(); Val f = a1(); Val as = a2(); Val nr = a3(); Val fr = a4(); line("call $vayu_raster_mat_perspective(l " + h.ssa + ", l " + f.ssa + ", l " + as.ssa + ", l " + nr.ssa + ", l " + fr.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mat_look_at") { Val h = a0(); Val ex = a1(); Val ey = a2(); Val ez = a3(); Val tx = a4(); Val ty = a5(); Val tz = a6(); Val ux = emitExpr(n->args[7].value.get()); Val uy = emitExpr(n->args[8].value.get()); Val uz = emitExpr(n->args[9].value.get()); line("call $vayu_raster_mat_look_at(l " + h.ssa + ", l " + ex.ssa + ", l " + ey.ssa + ", l " + ez.ssa + ", l " + tx.ssa + ", l " + ty.ssa + ", l " + tz.ssa + ", l " + ux.ssa + ", l " + uy.ssa + ", l " + uz.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "mesh_new") { std::string t = newTemp(); line(t + " =l call $vayu_raster_mesh_new()");   r.ssa = t; r.type = VType::Int; return r; }
                if (m == "mesh_free") { Val h = a0(); line("call $vayu_raster_mesh_free(l " + h.ssa + ")");       r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mesh_clear") { Val h = a0(); line("call $vayu_raster_mesh_clear(l " + h.ssa + ")");      r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mesh_add_vert") { Val h = a0(); Val x = a1(); Val y = a2(); Val z = a3(); Val u = a4(); Val v = a5(); line("call $vayu_raster_mesh_add_vert(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + z.ssa + ", l " + u.ssa + ", l " + v.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mesh_add_tri") { Val h = a0(); Val i0 = a1(); Val i1 = a2(); Val i2 = a3(); line("call $vayu_raster_mesh_add_tri(l " + h.ssa + ", l " + i0.ssa + ", l " + i1.ssa + ", l " + i2.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "draw_mesh") { Val fb = a0(); Val msh = a1(); Val mt = a2(); Val tx = a3(); Val tn = a4(); line("call $vayu_raster_draw_mesh(l " + fb.ssa + ", l " + msh.ssa + ", l " + mt.ssa + ", l " + tx.ssa + ", l " + tn.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "tex_new") { Val w = a0(); Val h = a1(); std::string t = newTemp(); line(t + " =l call $vayu_raster_tex_new(l " + w.ssa + ", l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "tex_free") { Val h = a0(); line("call $vayu_raster_tex_free(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "tex_width") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_raster_tex_width(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "tex_height") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_raster_tex_height(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "tex_set") { Val h = a0(); Val x = a1(); Val y = a2(); Val c = a3(); line("call $vayu_raster_tex_set(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "tex_set_filter") { Val h = a0(); Val mm = a1(); line("call $vayu_raster_tex_set_filter(l " + h.ssa + ", l " + mm.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "tex_gen_mipmaps") { Val h = a0(); line("call $vayu_raster_tex_gen_mipmaps(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "mesh_add_vert_lit") { Val h = a0();Val x = a1();Val y = a2();Val z = a3();Val nx = a4();Val ny = a5();Val nz = a6();Val u = emitExpr(n->args[7].value.get());Val v = emitExpr(n->args[8].value.get()); line("call $vayu_raster_mesh_add_vert_lit(l " + h.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + z.ssa + ", l " + nx.ssa + ", l " + ny.ssa + ", l " + nz.ssa + ", l " + u.ssa + ", l " + v.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "mesh_set_normal") { Val h = a0();Val i = a1();Val nx = a2();Val ny = a3();Val nz = a4(); line("call $vayu_raster_mesh_set_normal(l " + h.ssa + ", l " + i.ssa + ", l " + nx.ssa + ", l " + ny.ssa + ", l " + nz.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "set_ambient") { Val a = a0(); line("call $vayu_raster_set_ambient(l " + a.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "light_clear") { line("call $vayu_raster_light_clear()"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "light_set") { Val i = a0();Val k = a1();Val x = a2();Val y = a3();Val z = a4();Val c = a5();Val q = a6(); line("call $vayu_raster_light_set(l " + i.ssa + ", l " + k.ssa + ", l " + x.ssa + ", l " + y.ssa + ", l " + z.ssa + ", l " + c.ssa + ", l " + q.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "draw_mesh_lit") { Val fb = a0();Val msh = a1();Val mvp = a2();Val mod = a3();Val tx = a4();Val sm = a5();Val tn = a6();Val ex = emitExpr(n->args[7].value.get());Val ey = emitExpr(n->args[8].value.get());Val ez = emitExpr(n->args[9].value.get()); line("call $vayu_raster_draw_mesh_lit(l " + fb.ssa + ", l " + msh.ssa + ", l " + mvp.ssa + ", l " + mod.ssa + ", l " + tx.ssa + ", l " + sm.ssa + ", l " + tn.ssa + ", l " + ex.ssa + ", l " + ey.ssa + ", l " + ez.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "post_gamma") { Val t = a0();Val g = a1(); line("call $vayu_raster_post_gamma(l " + t.ssa + ", l " + g.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "post_invert") { Val t = a0(); line("call $vayu_raster_post_invert(l " + t.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "post_tint") { Val t = a0();Val c = a1(); line("call $vayu_raster_post_tint(l " + t.ssa + ", l " + c.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "post_brightness") { Val t = a0();Val d = a1(); line("call $vayu_raster_post_brightness(l " + t.ssa + ", l " + d.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "post_threshold") { Val t = a0();Val th = a1(); line("call $vayu_raster_post_threshold(l " + t.ssa + ", l " + th.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "tex_from_fb") { Val fb = a0(); std::string t = newTemp(); line(t + " =l call $vayu_raster_tex_from_fb(l " + fb.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "tex_present") { Val t = a0();Val c = a1();Val x = a2();Val y = a3(); line("call $vayu_raster_tex_present(l " + t.ssa + ", l " + c.ssa + ", l " + x.ssa + ", l " + y.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                throw std::runtime_error("native: raster has no method '" + m + "'");
            }

            Val emitTensorCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "new") { Val sh = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_new(l " + sh.ssa + ")");           r.ssa = t; r.type = VType::Int; return r; }
                if (m == "zeros") { Val sh = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_zeros(l " + sh.ssa + ")");         r.ssa = t; r.type = VType::Int; return r; }
                if (m == "ones") { Val sh = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_ones(l " + sh.ssa + ")");          r.ssa = t; r.type = VType::Int; return r; }
                if (m == "from_int_list") { Val sh = a0();Val vl = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_from_int_list(l " + sh.ssa + ", l " + vl.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "from_float_list") { Val sh = a0();Val vl = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_from_float_list(l " + sh.ssa + ", l " + vl.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "copy") { Val h = a0();  std::string t = newTemp(); line(t + " =l call $vayu_tensor_copy(l " + h.ssa + ")");           r.ssa = t; r.type = VType::Int; return r; }
                if (m == "free") { Val h = a0();  line("call $vayu_tensor_free(l " + h.ssa + ")");   r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "fill") { Val h = a0();Val v = a1(); line("call $vayu_tensor_fill(l " + h.ssa + ", l " + v.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "ndim") { Val h = a0();  std::string t = newTemp(); line(t + " =l call $vayu_tensor_ndim(l " + h.ssa + ")");           r.ssa = t; r.type = VType::Int; return r; }
                if (m == "shape") { Val h = a0();Val i = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_shape(l " + h.ssa + ", l " + i.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "numel") { Val h = a0();  std::string t = newTemp(); line(t + " =l call $vayu_tensor_numel_h(l " + h.ssa + ")");        r.ssa = t; r.type = VType::Int; return r; }
                if (m == "get") { Val h = a0();Val i = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_get_flat(l " + h.ssa + ", l " + i.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "set") { Val h = a0();Val i = a1();Val v = emitExpr(n->args[2].value.get()); line("call $vayu_tensor_set_flat(l " + h.ssa + ", l " + i.ssa + ", l " + v.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "add") { Val a = a0();Val b = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_add(l " + a.ssa + ", l " + b.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "sub") { Val a = a0();Val b = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_sub(l " + a.ssa + ", l " + b.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "mul") { Val a = a0();Val b = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_mul(l " + a.ssa + ", l " + b.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "div") { Val a = a0();Val b = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_div(l " + a.ssa + ", l " + b.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "add_scalar") { Val h = a0();Val v = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_add_scalar(l " + h.ssa + ", l " + v.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "mul_scalar") { Val h = a0();Val v = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_mul_scalar(l " + h.ssa + ", l " + v.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "matmul") { Val a = a0();Val b = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_matmul(l " + a.ssa + ", l " + b.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "sum") { Val h = a0();  std::string t = newTemp(); line(t + " =l call $vayu_tensor_sum(l " + h.ssa + ")");           r.ssa = t; r.type = VType::Int; return r; }
                if (m == "max") { Val h = a0();  std::string t = newTemp(); line(t + " =l call $vayu_tensor_max(l " + h.ssa + ")");           r.ssa = t; r.type = VType::Int; return r; }
                if (m == "argmax") { Val h = a0();  std::string t = newTemp(); line(t + " =l call $vayu_tensor_argmax(l " + h.ssa + ")");        r.ssa = t; r.type = VType::Int; return r; }
                if (m == "reshape") { Val h = a0();Val sh = a1(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_reshape(l " + h.ssa + ", l " + sh.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "transpose") { Val h = a0();  std::string t = newTemp(); line(t + " =l call $vayu_tensor_transpose(l " + h.ssa + ")");      r.ssa = t; r.type = VType::Int; return r; }
                if (m == "print") { Val h = a0();Val nm = a1(); line("call $vayu_tensor_print(l " + h.ssa + ", l " + nm.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                if (m == "requires_grad") { Val h = a0();Val f = a1(); line("call $vayu_tensor_requires_grad(l " + h.ssa + ", l " + f.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "zero_grad") { Val h = a0(); line("call $vayu_tensor_zero_grad(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "grad") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_grad(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "backward") { Val h = a0(); line("call $vayu_tensor_backward(l " + h.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "tape_clear") { line("call $vayu_tensor_tape_clear()"); r.ssa = "0"; r.type = VType::Void; return r; }
                if (m == "tape_size") { std::string t = newTemp(); line(t + " =l call $vayu_tensor_tape_size()"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "relu") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_relu(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "sigmoid") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_sigmoid(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "tanh") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_tanh(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "exp") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_exp(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "log") { Val h = a0(); std::string t = newTemp(); line(t + " =l call $vayu_tensor_log(l " + h.ssa + ")"); r.ssa = t; r.type = VType::Int; return r; }
                if (m == "copy_into") { Val d = a0();Val s = a1(); line("call $vayu_tensor_copy_into(l " + d.ssa + ", l " + s.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                throw std::runtime_error("native: tensor has no method '" + m + "'");
            }

            Val emitOnnxCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                auto a0 = [&]() { return emitExpr(n->args[0].value.get()); };
                auto a1 = [&]() { return emitExpr(n->args[1].value.get()); };

                if (m == "run_str") {
                    Val t = a0(); Val i = a1();
                    std::string tmp = newTemp();
                    line(tmp + " =l call $vayu_nn_run_str(l " + t.ssa + ", l " + i.ssa + ")");
                    r.ssa = tmp; r.type = VType::Int; return r;
                }
                if (m == "load") {
                    Val p = a0(); std::string t = newTemp();
                    line(t + " =l call $vayu_onnx_load(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return r;
                }
                if (m == "free") {
                    Val h = a0(); line("call $vayu_onnx_free(l " + h.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void; return r;
                }
                if (m == "run") {
                    Val mh = a0(); Val ih = a1(); std::string t = newTemp();
                    line(t + " =l call $vayu_onnx_run(l " + mh.ssa + ", l " + ih.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return r;
                }
                if (m == "input_name") {
                    Val h = a0(); std::string t = newTemp();
                    line(t + " =l call $vayu_onnx_input_name(l " + h.ssa + ")");
                    r.ssa = t; r.type = VType::Str; return r;
                }
                if (m == "output_name") {
                    Val h = a0(); std::string t = newTemp();
                    line(t + " =l call $vayu_onnx_output_name(l " + h.ssa + ")");
                    r.ssa = t; r.type = VType::Str; return r;
                }
                if (m == "debug") {
                    Val h = a0();
                    line("call $vayu_onnx_debug(l " + h.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void; return r;
                }

                throw std::runtime_error("native: onnx has no method '" + m + "'");
            }

            Val emitMathCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;

                auto argBits = [&](size_t i) -> std::string {
                    if (i >= n->args.size())
                        throw std::runtime_error("native: math." + m + ": too few args");
                    Val v = emitExpr(n->args[i].value.get());
                    if (v.type == VType::Float) return v.ssa;
                    if (v.type == VType::Int || v.type == VType::Bool) {
                        std::string d = newTemp();
                        line(d + " =d sltof " + v.ssa);
                        std::string rl = newTemp();
                        line(rl + " =l cast " + d);
                        return rl;
                    }
                    throw std::runtime_error(
                        "native: math." + m + ": unsupported argument type");
                    };

                /* floor / ceil / trunc return Int in tree-walk.  Handle
                   them here before the unary map so the result is
                   converted with dtosi. */
                if (m == "floor" || m == "ceil" || m == "trunc") {
                    std::string a = argBits(0);
                    std::string df = newTemp();
                    line(df + " =l call $vayu_math_" + m + "(l " + a + ")");
                    std::string dd = newTemp();
                    line(dd + " =d cast " + df);
                    std::string t = newTemp();
                    line(t + " =l dtosi " + dd);
                    r.ssa = t; r.type = VType::Int; return r;
                }

                static const std::unordered_map<std::string, const char*> unary = {
                    {"sqrt","$vayu_math_sqrt"},  {"sin","$vayu_math_sin"},
                    {"cos","$vayu_math_cos"},    {"tan","$vayu_math_tan"},
                    {"asin","$vayu_math_asin"},  {"acos","$vayu_math_acos"},
                    {"atan","$vayu_math_atan"},  {"sinh","$vayu_math_sinh"},
                    {"cosh","$vayu_math_cosh"},  {"tanh","$vayu_math_tanh"},
                    {"asinh","$vayu_math_asinh"},{"acosh","$vayu_math_acosh"},
                    {"atanh","$vayu_math_atanh"},{"exp","$vayu_math_exp"},
                    {"log","$vayu_math_log"},    {"log2","$vayu_math_log2"},
                    {"log10","$vayu_math_log10"},
                    {"fabs","$vayu_math_fabs"},  {"cbrt","$vayu_math_cbrt"},
                    {"expm1","$vayu_math_expm1"},{"log1p","$vayu_math_log1p"},
                    {"tgamma","$vayu_math_tgamma"},{"lgamma","$vayu_math_lgamma"},
                    {"erf","$vayu_math_erf"},    {"erfc","$vayu_math_erfc"},
                    {"degrees","$vayu_math_degrees"},
                    {"radians","$vayu_math_radians"},
                };
                auto u = unary.find(m);
                if (u != unary.end()) {
                    std::string a = argBits(0);
                    std::string t = newTemp();
                    line(t + " =l call " + std::string(u->second) + "(l " + a + ")");
                    r.ssa = t; r.type = VType::Float; return r;
                }

                static const std::unordered_map<std::string, const char*> binary = {
                    {"pow","$vayu_math_pow"},     {"atan2","$vayu_math_atan2"},
                    {"hypot","$vayu_math_hypot"}, {"fmod","$vayu_math_fmod"},
                    {"copysign","$vayu_math_copysign"},
                };
                auto b = binary.find(m);
                if (b != binary.end()) {
                    std::string a = argBits(0);
                    std::string c = argBits(1);
                    std::string t = newTemp();
                    line(t + " =l call " + std::string(b->second) + "(l " + a + ", l " + c + ")");
                    r.ssa = t; r.type = VType::Float; return r;
                }

                if (m == "is_nan" || m == "is_inf" || m == "is_finite") {
                    std::string a = argBits(0);
                    const char* fn = "$vayu_math_is_nan";
                    if (m == "is_inf") fn = "$vayu_math_is_inf";
                    else if (m == "is_finite") fn = "$vayu_math_is_finite";
                    std::string t = newTemp();
                    line(t + " =l call " + std::string(fn) + "(l " + a + ")");
                    r.ssa = t; r.type = VType::Bool; return r;
                }

                throw std::runtime_error("native: math has no method '" + m + "'");
            }

            Val emitCudaCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                if (m == "available") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_cuda_available()");
                    r.ssa = t; r.type = VType::Bool; return r;
                }
                if (m == "device_count") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_cuda_device_count()");
                    r.ssa = t; r.type = VType::Int; return r;
                }
                if (m == "matmul") {
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_cuda_matmul(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return r;
                }
                if (m == "shutdown") {
                    line("call $vayu_cuda_shutdown()");
                    r.ssa = "0"; r.type = VType::Void; return r;
                }
                throw std::runtime_error("native: cuda has no method '" + m + "'");
            }

            Val emitDmlCall(const CallExpr* n, const AttrExpr* attr) {
                Val r;
                const std::string& m = attr->name;
                if (m == "available") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_dml_available()");
                    r.ssa = t; r.type = VType::Bool; return r;
                }
                if (m == "matmul") {
                    Val a = emitExpr(n->args[0].value.get());
                    Val b = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_dml_matmul(l " + a.ssa + ", l " + b.ssa + ")");
                    r.ssa = t; r.type = VType::Int; return r;
                }
                if (m == "shutdown") {
                    line("call $vayu_dml_shutdown()");
                    r.ssa = "0"; r.type = VType::Void; return r;
                }
                throw std::runtime_error("native: dml has no method '" + m + "'");
            }

            Val emitCall(const CallExpr* n) {
                Val r;

                if (n->callee->kind == ExprKind::Attr) {
                    auto* attr = static_cast<const AttrExpr*>(n->callee.get());

                    if (attr->target->kind == ExprKind::Call) {
                        auto* inner = static_cast<const CallExpr*>(attr->target.get());
                        if (inner->callee->kind == ExprKind::NameRef &&
                            inner->args.empty()) {
                            const auto* inm = static_cast<const NameRefExpr*>(
                                inner->callee.get());
                            if (inm->name == "super") {
                                if (currentClass_.empty())
                                    throw std::runtime_error(
                                        "native: super() outside method");
                                const ClassInfo* current = findClass(currentClass_);
                                if (!current)
                                    throw std::runtime_error(
                                        "native: unknown current class '" +
                                        currentClass_ + "'");
                                if (!current->parent)
                                    throw std::runtime_error(
                                        "native: super() called in class '" +
                                        currentClass_ + "' which has no parent");

                                const ClassInfo* defining =
                                    findClassDefiningMethod(current->parent, attr->name);
                                if (!defining)
                                    throw std::runtime_error(
                                        "native: parent class '" +
                                        current->parent->name + "' has no method '" +
                                        attr->name + "'");

                                auto selfIt = slots_.find("self");
                                if (selfIt == slots_.end())
                                    throw std::runtime_error(
                                        "native: super() requires 'self' in scope");

                                std::string selfVal = newTemp();
                                line(selfVal + " =l loadl " + selfIt->second);

                                std::vector<std::string> args;
                                args.push_back(selfVal);
                                for (auto& a : n->args) {
                                    if (!a.name.empty())
                                        throw std::runtime_error(
                                            "native: kwargs not supported");
                                    args.push_back(emitExpr(a.value.get()).ssa);
                                }
                                std::string argsStr;
                                for (size_t i = 0; i < args.size(); ++i) {
                                    if (i) argsStr += ", ";
                                    argsStr += "l " + args[i];
                                }
                                std::string sym = "$vayu_mth_" + mangle(defining->name) +
                                    "_" + mangle(attr->name);
                                std::string t = newTemp();
                                line(t + " =l call " + sym + "(" + argsStr + ")");
                                r.ssa = t;

                                if (attr->name == "__init__") {
                                    r.type = VType::Void;
                                }
                                else {
                                    auto mit = defining->methods.find(attr->name);
                                    if (mit != defining->methods.end() &&
                                        mit->second->returnType) {
                                        Val tmp;
                                        inferFromAnnotation(
                                            mit->second->returnType.get(), tmp);
                                        r.type = tmp.type;
                                        r.cls = tmp.cls;
                                        r.elemType = tmp.elemType;
                                        r.elemCls = tmp.elemCls;
                                        r.valType = tmp.valType;
                                    }
                                    else {
                                        r.type = VType::Unknown;
                                    }
                                }
                                return r;
                            }
                        }
                    }

                    if (attr->target->kind == ExprKind::NameRef) {
                        const auto* tn0 = static_cast<const NameRefExpr*>(
                            attr->target.get());
                        if (tn0->name == "gui")   return emitGuiCall(n, attr);
                        if (tn0->name == "raster")return emitRasterCall(n, attr);
                        if (tn0->name == "tensor")return emitTensorCall(n, attr);
                        if (tn0->name == "onnx")  return emitOnnxCall(n, attr);
                        if (tn0->name == "cuda")  return emitCudaCall(n, attr);
                        if (tn0->name == "dml")   return emitDmlCall(n, attr);
                        if (tn0->name == "math")  return emitMathCall(n, attr);
                        if (tn0->name == "fs")    return emitFsCall(n, attr);
                        if (tn0->name == "time")  return emitTimeCall(n, attr);
                        if (tn0->name == "json")  return emitJsonCall(n, attr);
                        if (tn0->name == "regex") return emitRegexCall(n, attr);
                        if (tn0->name == "thread")return emitThreadCall(n, attr);
                        if (tn0->name == "net")   return emitNetCall(n, attr);
                        if (tn0->name == "crypto")return emitCryptoCall(n, attr);
                        if (tn0->name == "random")return emitRandomCall(n, attr);
                        if (tn0->name == "os")    return emitOsCall(n, attr);
                        if (tn0->name == "py") {
                            // Phase 16.0: intercept py.method(...)
                            const std::string& m = attr->name;
                            if (m == "init") {
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_init()");
                                r.ssa = t; r.type = VType::Bool;
                                return r;
                            }
                            if (m == "version") {
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_version()");
                                r.ssa = t; r.type = VType::Str;
                                return r;
                            }
                            if (m == "run" || m == "exec") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py." + m +
                                        "() takes one str argument");
                                Val s = emitExpr(n->args[0].value.get());
                                if (s.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py." + m +
                                        "() argument must be str");
                                std::string c = newTemp();
                                line(c + " =l call $vayu_str_cstr(l " +
                                    s.ssa + ")");
                                line("call $vayu_py_run(l " + c + ")");
                                r.ssa = "0"; r.type = VType::Void;
                                return r;
                            }
                            // ---- Phase 16.1: value bridging ----
                            if (m == "int") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.int() takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                if (v.type != VType::Int &&
                                    v.type != VType::Bool)
                                    throw std::runtime_error(
                                        "native: py.int() argument must be "
                                        "int or bool");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_int(l " +
                                    v.ssa + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "str") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.str() takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                if (v.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py.str() argument must be str");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_str(l " +
                                    v.ssa + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "bool") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.bool() takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string w = newTemp();
                                line(w + " =w cnel " + v.ssa + ", 0");
                                std::string ext = newTemp();
                                line(ext + " =l extsw " + w);
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_bool(l " +
                                    ext + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "from_int") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.from_int() takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_from_int(l " +
                                    v.ssa + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "from_str") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.from_str() takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_from_str(l " +
                                    v.ssa + ")");
                                r.ssa = t; r.type = VType::Str;
                                return r;
                            }
                            if (m == "from_bool") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.from_bool() takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_from_bool(l " +
                                    v.ssa + ")");
                                r.ssa = t; r.type = VType::Bool;
                                return r;
                            }
                            // ---- Phase 16.2: calling Python ----
                            if (m == "import") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.import() takes one str argument");
                                Val v = emitExpr(n->args[0].value.get());
                                if (v.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py.import() argument must be str");
                                std::string c = newTemp();
                                line(c + " =l call $vayu_str_cstr(l " +
                                    v.ssa + ")");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_import(l " + c + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "getattr") {
                                if (n->args.size() != 2)
                                    throw std::runtime_error(
                                        "native: py.getattr(obj, name) takes two arguments");
                                Val o = emitExpr(n->args[0].value.get());
                                Val v = emitExpr(n->args[1].value.get());
                                if (v.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py.getattr() name must be str");
                                std::string c = newTemp();
                                line(c + " =l call $vayu_str_cstr(l " +
                                    v.ssa + ")");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_getattr(l " +
                                    o.ssa + ", l " + c + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "call") {
                                if (n->args.size() != 2)
                                    throw std::runtime_error(
                                        "native: py.call(fn, args) takes two arguments");
                                Val f = emitExpr(n->args[0].value.get());
                                Val a = emitExpr(n->args[1].value.get());
                                if (a.type != VType::List)
                                    throw std::runtime_error(
                                        "native: py.call() second arg must be a list");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_call(l " +
                                    f.ssa + ", l " + a.ssa + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "decref") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.decref() takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                line("call $vayu_py_decref(l " + v.ssa + ")");
                                r.ssa = "0"; r.type = VType::Void;
                                return r;
                            }
                            // ---- Phase 16.3: bidirectional ----
                            if (m == "eval") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.eval() takes one str argument");
                                Val v = emitExpr(n->args[0].value.get());
                                if (v.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py.eval() argument must be str");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_eval(l " + v.ssa + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "exec_file") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.exec_file() takes one str argument");
                                Val v = emitExpr(n->args[0].value.get());
                                if (v.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py.exec_file() argument must be str");
                                line("call $vayu_py_exec_file(l " + v.ssa + ")");
                                r.ssa = "0"; r.type = VType::Void;
                                return r;
                            }
                            if (m == "callback") {
                                if (n->args.size() != 2)
                                    throw std::runtime_error(
                                        "native: py.callback(fn, ret_kind) "
                                        "takes two arguments");
                                Val f = emitExpr(n->args[0].value.get());
                                Val k = emitExpr(n->args[1].value.get());
                                if (k.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py.callback ret_kind must be str");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_callback(l " +
                                    f.ssa + ", l " + k.ssa + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            // ---- Phase 16.4: attributes / repr / type_name ----
                            if (m == "setattr") {
                                if (n->args.size() != 3)
                                    throw std::runtime_error(
                                        "native: py.setattr(obj, name, value) "
                                        "takes three arguments");
                                Val o = emitExpr(n->args[0].value.get());
                                Val k = emitExpr(n->args[1].value.get());
                                Val v = emitExpr(n->args[2].value.get());
                                if (k.type != VType::Str)
                                    throw std::runtime_error(
                                        "native: py.setattr name must be str");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_setattr(l " +
                                    o.ssa + ", l " + k.ssa + ", l " + v.ssa + ")");
                                r.ssa = t; r.type = VType::Bool;
                                return r;
                            }
                            if (m == "repr") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.repr(obj) takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_repr(l " + v.ssa + ")");
                                std::string t2 = newTemp();
                                line(t2 + " =l call $vayu_py_from_str(l " + t + ")");
                                r.ssa = t2; r.type = VType::Str;
                                return r;
                            }
                            if (m == "type_name") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.type_name(obj) takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_type_name(l " + v.ssa + ")");
                                r.ssa = t; r.type = VType::Str;
                                return r;
                            }
                            // ---- Phase 16.5: kwargs / list / dict / last_error ----
                            if (m == "call_kw") {
                                if (n->args.size() != 2)
                                    throw std::runtime_error(
                                        "native: py.call_kw(fn, kwargs) "
                                        "takes two arguments");
                                Val f = emitExpr(n->args[0].value.get());
                                Val kw = emitExpr(n->args[1].value.get());
                                if (kw.type != VType::Map)
                                    throw std::runtime_error(
                                        "native: py.call_kw kwargs must be a map");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_call_kw(l " +
                                    f.ssa + ", l " + kw.ssa + ")");
                                r.ssa = t; r.type = VType::Int;
                                return r;
                            }
                            if (m == "list") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.list(obj) takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_list(l " + v.ssa + ")");
                                r.ssa = t; r.type = VType::List;
                                r.elemType = VType::Int;
                                return r;
                            }
                            if (m == "dict") {
                                if (n->args.size() != 1)
                                    throw std::runtime_error(
                                        "native: py.dict(obj) takes one argument");
                                Val v = emitExpr(n->args[0].value.get());
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_dict(l " + v.ssa + ")");
                                r.ssa = t; r.type = VType::Map;
                                r.elemType = VType::Str;
                                r.valType = VType::Int;
                                return r;
                            }
                            if (m == "last_error") {
                                if (!n->args.empty())
                                    throw std::runtime_error(
                                        "native: py.last_error() takes no arguments");
                                std::string t = newTemp();
                                line(t + " =l call $vayu_py_last_error()");
                                r.ssa = t; r.type = VType::Str;
                                return r;
                            }
                            throw std::runtime_error(
                                "native: py has no method '" + m + "'");
                        }
                    }

                    if (attr->target->kind == ExprKind::NameRef) {
                        const auto* tn = static_cast<const NameRefExpr*>(attr->target.get());
                        if (modules_.count(tn->name)) {
                            std::vector<std::string> args;
                            for (auto& a : n->args) {
                                if (!a.name.empty())
                                    throw std::runtime_error("native: kwargs not supported");
                                args.push_back(emitExpr(a.value.get()).ssa);
                            }
                            std::string argsStr;
                            for (size_t i = 0; i < args.size(); ++i) {
                                if (i) argsStr += ", ";
                                argsStr += "l " + args[i];
                            }
                            std::string sym = "$vayu_fn_" + mangle(tn->name)
                                + "_" + mangle(attr->name);
                            std::string t = newTemp();
                            line(t + " =l call " + sym + "(" + argsStr + ")");
                            r.ssa = t; r.type = VType::Unknown;
                            return r;
                        }
                    }

                    Val recv = emitExpr(attr->target.get());

                    if (tryBuiltinMethod(attr->name, recv, n, r)) return r;

                    if (recv.type == VType::Obj) {
                        auto ci = findClass(recv.cls);
                        if (!ci) throw std::runtime_error("native: unknown class");
                        const ClassInfo* defining =
                            findClassDefiningMethod(ci, attr->name);
                        if (!defining)
                            throw std::runtime_error(
                                "native: class '" + recv.cls + "' has no method '" +
                                attr->name + "'");

                        std::vector<std::string> args;
                        args.push_back(recv.ssa);
                        for (auto& a : n->args) {
                            if (!a.name.empty())
                                throw std::runtime_error(
                                    "native: kwargs not supported");
                            args.push_back(emitExpr(a.value.get()).ssa);
                        }
                        std::string argsStr;
                        for (size_t i = 0; i < args.size(); ++i) {
                            if (i) argsStr += ", ";
                            argsStr += "l " + args[i];
                        }
                        std::string sym = "$vayu_mth_" + mangle(defining->name) +
                            "_" + mangle(attr->name);
                        std::string t = newTemp();
                        line(t + " =l call " + sym + "(" + argsStr + ")");
                        r.ssa = t;

                        if (attr->name == "__init__") {
                            r.type = VType::Void;
                        }
                        else {
                            auto mit = defining->methods.find(attr->name);
                            if (mit != defining->methods.end() &&
                                mit->second->returnType) {
                                Val tmp;
                                inferFromAnnotation(
                                    mit->second->returnType.get(), tmp);
                                r.type = tmp.type;
                                r.cls = tmp.cls;
                                r.elemType = tmp.elemType;
                                r.elemCls = tmp.elemCls;
                                r.valType = tmp.valType;
                            }
                            else {
                                r.type = VType::Unknown;
                            }
                        }
                        return r;
                    }

                    if (attr->target->kind == ExprKind::NameRef) {
                        const auto* tn = static_cast<const NameRefExpr*>(
                            attr->target.get());
                        if (modules_.count(tn->name)) {
                            std::vector<std::string> args;
                            for (auto& a : n->args)
                                args.push_back(emitExpr(a.value.get()).ssa);
                            std::string argsStr;
                            for (size_t i = 0; i < args.size(); ++i) {
                                if (i) argsStr += ", ";
                                argsStr += "l " + args[i];
                            }
                            std::string sym = "$vayu_fn_" + mangle(tn->name) + "_" +
                                mangle(attr->name);
                            std::string t = newTemp();
                            line(t + " =l call " + sym + "(" + argsStr + ")");
                            r.ssa = t; r.type = VType::Unknown;
                            return r;
                        }
                    }

                    throw std::runtime_error(
                        "native: method call on unsupported value");
                }

                if (n->callee->kind != ExprKind::NameRef)
                    throw std::runtime_error("native: indirect calls not supported");

                const auto* nm = static_cast<const NameRefExpr*>(n->callee.get());
                std::string name = nm->name;
                // Phase 15.1: extern "C" call.
                auto exIt = externDecls_.find(name);
                if (exIt != externDecls_.end()) {
                    const ExternFnDecl* ed = exIt->second;
                    if (n->args.size() != ed->params.size())
                        throw std::runtime_error(
                            "native: extern '" + name + "' expects " +
                            std::to_string(ed->params.size()) +
                            " argument(s), got " +
                            std::to_string(n->args.size()));

                    std::vector<std::string> args;
                    for (size_t i = 0; i < n->args.size(); ++i) {
                        Val a = emitExpr(n->args[i].value.get());
                        std::string argSsa = a.ssa;
                        const Expr* pType = ed->params[i].type.get();

                        // str → char*
                        bool wantStr = (pType &&
                            pType->kind == ExprKind::NameRef &&
                            static_cast<const NameRefExpr*>(pType)->name == "str");
                        if (wantStr && a.type == VType::Str) {
                            std::string t = newTemp();
                            line(t + " =l call $vayu_str_cstr(l " +
                                a.ssa + ")");
                            argSsa = t;
                        }

                        // Phase 15.5: class-typed parameter.
                        // ≤ 8 bytes → unwrap the single field and pass by
                        // value.  Larger → pass the pointer as-is.
                        if (pType && pType->kind == ExprKind::NameRef) {
                            const std::string& pn =
                                static_cast<const NameRefExpr*>(pType)->name;
                            const ClassInfo* ci = findClass(pn);
                            if (ci && !ci->fields.empty() &&
                                ci->totalSize <= 8) {
                                int off = ci->fieldOffsets.at(ci->fields[0]);
                                std::string addr = newTemp();
                                line(addr + " =l add " + a.ssa + ", " +
                                    std::to_string(off));
                                std::string field = newTemp();
                                line(field + " =l loadl " + addr);
                                argSsa = field;
                            }
                        }

                        args.push_back(argSsa);
                    }
                    std::string argsStr;
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i) argsStr += ", ";
                        argsStr += "l " + args[i];
                    }
                    std::string t = newTemp();
                    line(t + " =l call $" + ed->name + "(" + argsStr + ")");

                    // ---- return type ----
                    VType rType = VType::Int;
                    std::string rCls;

                    if (ed->returnType) {
                        if (ed->returnType->kind == ExprKind::GenericType) {
                            auto* g = static_cast<const GenericTypeExpr*>(
                                ed->returnType.get());
                            if (g->name == "ptr") rType = VType::Ptr;
                        }
                        else if (ed->returnType->kind == ExprKind::NameRef) {
                            const std::string& rn =
                                static_cast<const NameRefExpr*>(
                                    ed->returnType.get())->name;
                            if (rn == "bool") rType = VType::Bool;
                            else if (rn == "str") {
                                std::string t2 = newTemp();
                                line(t2 + " =l call $vayu_cstr_to_str(l " +
                                    t + ")");
                                t = t2;
                                rType = VType::Str;
                            }
                            else if (classes_.count(rn)) {
                                const ClassInfo* ci = findClass(rn);
                                if (ci && !ci->fields.empty() &&
                                    ci->totalSize <= 8) {
                                    // By-value small struct return:
                                    // wrap the single i64 into a heap cell
                                    // shaped like the Vayu class.
                                    std::string cell = newTemp();
                                    line(cell + " =l call $vayu_alloc(l " +
                                        std::to_string(ci->totalSize) + ")");
                                    int off = ci->fieldOffsets.at(
                                        ci->fields[0]);
                                    std::string addr = newTemp();
                                    line(addr + " =l add " + cell + ", " +
                                        std::to_string(off));
                                    line("storel " + t + ", " + addr);
                                    t = cell;
                                }
                                // else: the C side returned a pointer.
                                rType = VType::Obj;
                                rCls = rn;
                            }
                        }
                    }
                    r.ssa = t; r.type = rType; r.cls = rCls;
                    return r;
                }

                // Phase 15.6: malloc / free fallback when no extern decl.
                if ((name == "malloc" || name == "free") &&
                    !externDecls_.count(name)) {
                    std::vector<std::string> args;
                    for (auto& a : n->args) {
                        if (!a.name.empty())
                            throw std::runtime_error(
                                "native: kwargs not supported");
                        args.push_back(emitExpr(a.value.get()).ssa);
                    }
                    std::string argsStr;
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i) argsStr += ", ";
                        argsStr += "l " + args[i];
                    }
                    std::string t = newTemp();
                    line(t + " =l call $" + name + "(" + argsStr + ")");
                    r.ssa = t;
                    r.type = (name == "malloc") ? VType::Ptr : VType::Void;
                    return r;
                }

                if (name == "print") {
                    if (n->args.empty()) {
                        line("call $vayu_print_ln()");
                        r.ssa = "0"; r.type = VType::Void; return r;
                    }
                    for (size_t i = 0; i < n->args.size(); ++i) {
                        Val v = emitExpr(n->args[i].value.get());
                        if (i > 0) line("call $vayu_print_space()");
                        switch (v.type) {
                        case VType::Bool:
                            line("call $vayu_print_bool_noln(l " + v.ssa + ")");
                            break;
                        case VType::Float:
                            line("call $vayu_print_float_noln(l " + v.ssa + ")");
                            break;
                        case VType::Str:
                            line("call $vayu_print_str_noln(l " + v.ssa + ")");
                            break;
                        case VType::List:
                            line("call $vayu_print_list_noln(l " + v.ssa + ")");
                            break;
                        case VType::Map:
                            line("call $vayu_print_map_noln(l " + v.ssa + ", l " +
                                std::to_string(kindOf(v.valType)) + ")");
                            break;
                        case VType::Tuple:
                            line("call $vayu_print_tuple_noln(l " + v.ssa + ")");
                            break;
                        case VType::Set:
                            line("call $vayu_print_set_noln(l " + v.ssa + ")");
                            break;
                        default:
                            line("call $vayu_print_int_noln(l " + v.ssa + ")");
                            break;
                        }
                    }
                    line("call $vayu_print_ln()");
                    r.ssa = "0"; r.type = VType::Void; return r;
                }

                if (name == "len") {
                    Val v = emitExpr(n->args[0].value.get());
                    int kind = -1;
                    switch (v.type) {
                    case VType::Str:  kind = 0; break;
                    case VType::List: kind = 1; break;
                    case VType::Map:  kind = 2; break;
                    case VType::Tuple: kind = 3; break;
                    case VType::Set:   kind = 4; break;
                    default: throw std::runtime_error(
                        "native: len() of unsupported type");
                    }
                    std::string t = newTemp();
                    line(t + " =l call $vayu_len(l " + v.ssa + ", l " +
                        std::to_string(kind) + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }

                if (name == "str") {
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Str) return v;
                    if (v.type == VType::Float) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_float_to_str(l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::Str;
                        return r;
                    }
                    if (v.type == VType::Int || v.type == VType::Bool) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_int_to_str(l " + v.ssa + ", l " +
                            std::to_string(kindOf(v.type)) + ")");
                        r.ssa = t; r.type = VType::Str;
                        return r;
                    }
                    throw std::runtime_error(
                        "native: str() unsupported on this type");
                }
                if (name == "int") {
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Int)  return v;
                    if (v.type == VType::Bool) return v;
                    if (v.type == VType::Float) {
                        std::string vd = newTemp();
                        line(vd + " =d cast " + v.ssa);
                        std::string t = newTemp();
                        line(t + " =l dtosi " + vd);
                        r.ssa = t; r.type = VType::Int;
                        return r;
                    }
                    if (v.type == VType::Str) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_to_int(l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::Int;
                        return r;
                    }
                    throw std::runtime_error(
                        "native: int() unsupported on this type");
                }

                if (name == "float") {
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Float) return v;
                    if (v.type == VType::Int || v.type == VType::Bool) {
                        std::string t = newTemp();
                        line(t + " =d sltof " + v.ssa);
                        std::string rl = newTemp();
                        line(rl + " =l cast " + t);
                        r.ssa = rl; r.type = VType::Float; return r;
                    }
                    if (v.type == VType::Str) {
                        std::string t = newTemp();
                        line(t + " =l call $vayu_str_to_float(l " + v.ssa + ")");
                        r.ssa = t; r.type = VType::Float; return r;
                    }
                    throw std::runtime_error(
                        "native: float() cannot convert value of type "
                        + std::to_string((int)v.type));
                }

                if (name == "bool") {
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Bool) return v;
                    std::string w = newTemp();
                    line(w + " =w cnel " + v.ssa + ", 0");
                    std::string ext = newTemp();
                    line(ext + " =l extsw " + w);
                    r.ssa = ext; r.type = VType::Bool;
                    return r;
                }

                if (tryPureBuiltin(name, n, r)) return r;

                if (name == "ord") {
                    Val v = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_ord(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }

                if (name == "chr") {
                    Val v = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_chr(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }

                if (name == "read_file") {
                    Val p = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_read_file(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }
                if (name == "read_line") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_read_line()");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }

                if (name == "read_all") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_read_all()");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }

                if (name == "read_int") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_read_int()");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }
                if (name == "input") {
                    if (n->args.size() > 1)
                        throw std::runtime_error(
                            "native: input() takes 0 or 1 argument");
                    std::string t = newTemp();
                    if (n->args.empty()) {
                        line(t + " =l call $vayu_input_plain()");
                    }
                    else {
                        Val p = emitExpr(n->args[0].value.get());
                        if (p.type != VType::Str)
                            throw std::runtime_error(
                                "native: input() prompt must be a string");
                        line(t + " =l call $vayu_input_prompt(l " + p.ssa + ")");
                    }
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }

                if (name == "print_raw") {
                    Val v = emitExpr(n->args[0].value.get());
                    if (v.type == VType::Str) {
                        line("call $vayu_print_raw(l " + v.ssa + ")");
                    }
                    else {
                        throw std::runtime_error(
                            "native: print_raw() requires a str argument");
                    }
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }

                if (name == "write_file") {
                    Val p = emitExpr(n->args[0].value.get());
                    Val c = emitExpr(n->args[1].value.get());
                    line("call $vayu_write_file(l " + p.ssa + ", l " + c.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }

                if (name == "file_exists") {
                    Val p = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_file_exists(l " + p.ssa + ")");
                    r.ssa = t; r.type = VType::Bool;
                    return r;
                }

                if (name == "args") {
                    std::string t = newTemp();
                    line(t + " =l call $vayu_get_args()");
                    r.ssa = t; r.type = VType::List; r.elemType = VType::Str;
                    return r;
                }

                if (name == "run_command") {
                    Val c = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_run_command(l " + c.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }

                if (name == "exit") {
                    Val c = emitExpr(n->args[0].value.get());
                    line("call $vayu_exit(l " + c.ssa + ")");
                    r.ssa = "0"; r.type = VType::Void;
                    return r;
                }

                if (name == "join") {
                    Val sep = emitExpr(n->args[0].value.get());
                    Val lst = emitExpr(n->args[1].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_str_join(l " + sep.ssa +
                        ", l " + lst.ssa + ")");
                    r.ssa = t; r.type = VType::Str;
                    return r;
                }

                if (name == "range")
                    throw std::runtime_error("native: range() only in `for` loops");

                if (isExceptionName(name)) {
                    std::string typeLbl = internString(name);
                    Val msg = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_mkexc(l " + typeLbl + ", l " + msg.ssa + ")");
                    r.ssa = t; r.type = VType::Exc;
                    return r;
                }

                if (classes_.count(name)) {
                    std::vector<std::string> args;
                    for (auto& a : n->args) {
                        if (!a.name.empty())
                            throw std::runtime_error("native: kwargs not supported");
                        args.push_back(emitExpr(a.value.get()).ssa);
                    }
                    std::string argsStr;
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i) argsStr += ", ";
                        argsStr += "l " + args[i];
                    }
                    std::string sym = "$vayu_ctor_" + mangle(name);
                    std::string t = newTemp();
                    line(t + " =l call " + sym + "(" + argsStr + ")");
                    r.ssa = t; r.type = VType::Obj; r.cls = name;
                    return r;
                }

                if (name == "next") {
                    if (n->args.size() != 1)
                        throw std::runtime_error(
                            "native: next() takes exactly one argument");
                    Val v = emitExpr(n->args[0].value.get());
                    std::string t = newTemp();
                    line(t + " =l call $vayu_gen_next(l " + v.ssa + ")");
                    r.ssa = t; r.type = VType::Int;
                    return r;
                }

                if (generatorFunctions_.count(name)) {
                    size_t argCount = n->args.size();
                    std::string argsStruct = newTemp();
                    line(argsStruct + " =l call $vayu_alloc(l " +
                        std::to_string((argCount == 0 ? 1 : argCount) * 8) + ")");
                    for (size_t i = 0; i < argCount; ++i) {
                        Val a = emitExpr(n->args[i].value.get());
                        std::string slot = newTemp();
                        line(slot + " =l add " + argsStruct + ", " +
                            std::to_string(i * 8));
                        line("storel " + a.ssa + ", " + slot);
                    }
                    std::string genSym =
                        "$vayu_gen_" + currentModulePrefix_ + mangle(name);
                    std::string fp = newTemp();
                    line(fp + " =l copy " + genSym);
                    std::string t = newTemp();
                    line(t + " =l call $vayu_gen_new(l " + fp +
                        ", l " + argsStruct + ")");
                    r.ssa = t; r.type = VType::Obj; r.cls = "Gen";
                    return r;
                }

                auto fi = fromImports_.find(name);
                auto fit = topFnDecls_.find(name);

                std::string sym;
                if (fi != fromImports_.end()) {
                    sym = "$vayu_fn_" + mangle(fi->second) + "_" + mangle(name);
                }
                else if (!currentModulePrefix_.empty() &&
                    currentModuleFnNames_.count(name)) {
                    sym = "$vayu_fn_" + currentModulePrefix_ + mangle(name);
                }
                else {
                    sym = "$vayu_fn_" + mangle(name);
                }

                /* If the target function declares a `float` parameter and
                   the caller passes an Int, widen it.  Without this, an
                   int bit pattern (0x0000...0001) gets reinterpreted as
                   the double 4.94e-324 and the callee produces garbage. */
                std::vector<std::string> args;
                for (size_t ai = 0; ai < n->args.size(); ++ai) {
                    const auto& a = n->args[ai];
                    if (!a.name.empty())
                        throw std::runtime_error("native: kwargs not supported");
                    Val v = emitExpr(a.value.get());
                    bool wantFloat = false;
                    if (fit != topFnDecls_.end() && ai < fit->second->params.size()) {
                        const Expr* pt = fit->second->params[ai].type.get();
                        if (pt && pt->kind == ExprKind::NameRef) {
                            const std::string& pn =
                                static_cast<const NameRefExpr*>(pt)->name;
                            if (pn == "float") wantFloat = true;
                        }
                    }
                    if (std::getenv("VAYU_DEBUG_WIDEN") != nullptr) {
                        std::fprintf(stderr,
                            "[widen] fn=%s arg=%zu wantFloat=%d vtype=%d ssa=%s\n",
                            name.c_str(), ai, wantFloat ? 1 : 0,
                            (int)v.type, v.ssa.c_str());
                    }
                    if (wantFloat && v.type == VType::Int) {
                        std::string d = newTemp();
                        line(d + " =d sltof " + v.ssa);
                        std::string rl = newTemp();
                        line(rl + " =l cast " + d);
                        args.push_back(rl);
                    }
                    else {
                        args.push_back(v.ssa);
                    }
                }
                std::string argsStr;
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i) argsStr += ", ";
                    argsStr += "l " + args[i];
                }
                std::string t = newTemp();
                line(t + " =l call " + sym + "(" + argsStr + ")");
                r.ssa = t;
                if (fit != topFnDecls_.end() && fit->second->returnType) {
                    bindTypeParamsFromCall(fit->second, n, r);

                    if (r.type == VType::List && r.elemType != VType::Unknown)
                        line("call $vayu_list_retag(l " + t + ", l " +
                            std::to_string(tagOf(r.elemType)) + ")");
                }
                else {
                    r.type = VType::Unknown;
                }
                return r;
            }

            bool emitStackAssignment(const AssignStmt* n) {
                if (n->target->kind != ExprKind::NameRef) return false;
                const std::string& lhs = static_cast<const NameRefExpr*>(
                    n->target.get())->name;
                auto ne = nonEscapingClasses_.find(lhs);
                if (ne == nonEscapingClasses_.end()) return false;
                if (n->value->kind != ExprKind::Call) return false;
                auto* c = static_cast<const CallExpr*>(n->value.get());
                if (c->callee->kind != ExprKind::NameRef) return false;

                const std::string& className = ne->second;
                const ClassInfo* ci = findClass(className);
                if (!ci) return false;

                std::string inst = "%" + mangle(lhs) + "_inst";
                std::vector<std::string> args;
                args.push_back(inst);
                for (auto& a : c->args) {
                    if (!a.name.empty())
                        throw std::runtime_error("native: kwargs not supported");
                    args.push_back(emitExpr(a.value.get()).ssa);
                }
                std::string argsStr;
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i) argsStr += ", ";
                    argsStr += "l " + args[i];
                }
                const ClassInfo* owner = ci->initOwner ? ci->initOwner : ci;
                std::string sym = "$vayu_mth_" + mangle(owner->name) + "_" +
                    mangle("__init__");
                line("call " + sym + "(" + argsStr + ")");
                return true;
            }

            void emitStmt(const Stmt* s) {
                if (!s) return;
                switch (s->kind) {
                case StmtKind::Expr: {
                    auto* n = static_cast<const ExprStmt*>(s);
                    emitExpr(n->expr.get());
                    return;
                }
                case StmtKind::Assign: {
                    auto* n = static_cast<const AssignStmt*>(s);

                    if (emitStackAssignment(n)) return;

                    if (n->target->kind == ExprKind::Attr) {
                        auto* attr = static_cast<const AttrExpr*>(n->target.get());
                        if (attr->target->kind == ExprKind::NameRef) {
                            const auto* tn = static_cast<const NameRefExpr*>(
                                attr->target.get());
                            auto sit = statics_.find(tn->name);
                            if (sit != statics_.end()) {
                                auto mit = sit->second.find(attr->name);
                                if (mit != sit->second.end()) {
                                    const std::string& slotName = mit->second;
                                    const std::string* slotPtr = nullptr;
                                    {
                                        auto git = slots_.find(slotName);
                                        if (git != slots_.end()) slotPtr = &git->second;
                                    }
                                    if (!slotPtr) {
                                        auto git2 = globalSlots_.find(slotName);
                                        if (git2 != globalSlots_.end()) slotPtr = &git2->second;
                                    }
                                    if (!slotPtr)
                                        throw std::runtime_error(
                                            "native: static '" + tn->name + "." +
                                            attr->name + "' not initialized");
                                    Val v = emitExpr(n->value.get());
                                    line("storel " + v.ssa + ", " + *slotPtr);
                                    VarInfo vi;
                                    vi.type = v.type; vi.clsName = v.cls;
                                    vi.elemType = v.elemType;
                                    vi.elemClsName = v.elemCls;
                                    vi.valType = v.valType;
                                    vi.tupleElemTypes = v.tupleElemTypes;
                                    vi.tupleElemClsNames = v.tupleElemClsNames;
                                    varInfo_[slotName] = vi;
                                    return;
                                }
                            }
                        }

                        Val recv = emitExpr(attr->target.get());
                        Val v = emitExpr(n->value.get());
                        if (recv.type != VType::Obj)
                            throw std::runtime_error(
                                "native: field assign on non-object");
                        const ClassInfo* ci = findClass(recv.cls);
                        int fieldOff = -1;
                        if (ci) {
                            auto fit = ci->fieldOffsets.find(attr->name);
                            if (fit != ci->fieldOffsets.end())
                                fieldOff = fit->second;
                        }
                        if (fieldOff < 0) {
                            auto git = fieldGlobals_.find(attr->name);
                            if (git != fieldGlobals_.end())
                                fieldOff = git->second;
                        }
                        if (fieldOff < 0)
                            throw std::runtime_error(
                                "native: '" + attr->name + "' is not a field");
                        std::string addr = newTemp();
                        line(addr + " =l add " + recv.ssa + ", " +
                            std::to_string(fieldOff));
                        line("storel " + v.ssa + ", " + addr);
                        return;
                    }
                    if (n->target->kind == ExprKind::Index) {
                        auto* ix = static_cast<const IndexExpr*>(n->target.get());
                        Val tgt = emitExpr(ix->target.get());
                        Val idx = emitExpr(ix->index.get());
                        Val v = emitExpr(n->value.get());
                        if (tgt.type == VType::Ptr) {
                            std::string scaled = newTemp();
                            line(scaled + " =l mul " + idx.ssa + ", 8");
                            std::string addr = newTemp();
                            line(addr + " =l add " + tgt.ssa + ", " + scaled);
                            line("storel " + v.ssa + ", " + addr);
                            return;
                        }
                        if (tgt.type == VType::List) {
                            line("call $vayu_list_set_tagged(l " + tgt.ssa +
                                ", l " + idx.ssa + ", l " + v.ssa + ", l " +
                                std::to_string(tagOf(v.type)) + ")");
                            return;
                        }
                        if (tgt.type == VType::Map) {
                            line("call $vayu_map_put(l " + tgt.ssa + ", l " +
                                idx.ssa + ", l " + v.ssa + ")");
                            return;
                        }
                        throw std::runtime_error(
                            "native: index-assign on unsupported type");
                    }
                    if (n->target->kind != ExprKind::NameRef)
                        if (n->target->kind == ExprKind::Unary) {
                            auto* u = static_cast<const UnaryExpr*>(n->target.get());
                            if (u->op == UnOp::Deref) {
                                Val p = emitExpr(u->operand.get());
                                Val v = emitExpr(n->value.get());
                                line("storel " + v.ssa + ", " + p.ssa);
                                return;
                            }
                            throw std::runtime_error(
                                "native: unsupported unary assignment target");
                        }

                    auto* nm = static_cast<const NameRefExpr*>(n->target.get());
                    std::string slotName = nm->name;
                    auto fi = fromImports_.find(slotName);
                    if (fi != fromImports_.end())
                        slotName = mangle(fi->second) + "_" + nm->name;
                    else if (!currentModulePrefix_.empty())
                        slotName = currentModulePrefix_ + nm->name;

                    Val v = emitExpr(n->value.get());
                    const std::string* slotPtr = nullptr;
                    {
                        auto it = slots_.find(slotName);
                        if (it != slots_.end()) slotPtr = &it->second;
                    }
                    if (!slotPtr && nm->name != slotName) {
                        auto it2 = slots_.find(nm->name);
                        if (it2 != slots_.end()) slotPtr = &it2->second;
                    }
                    if (!slotPtr) {
                        auto it3 = globalSlots_.find(nm->name);
                        if (it3 != globalSlots_.end()) slotPtr = &it3->second;
                    }
                    if (!slotPtr)
                        throw std::runtime_error(
                            "native: variable '" + nm->name + "' not declared");
                    line("storel " + v.ssa + ", " + *slotPtr);

                    VarInfo vi;
                    vi.type = v.type;
                    vi.clsName = v.cls;
                    vi.elemType = v.elemType;
                    vi.elemClsName = v.elemCls;
                    vi.valType = v.valType;
                    vi.tupleElemTypes = v.tupleElemTypes;
                    vi.tupleElemClsNames = v.tupleElemClsNames;
                    varInfo_[slotName] = vi;
                    return;
                }
                case StmtKind::AnnotAssign: {
                    auto* n = static_cast<const AnnotAssignStmt*>(s);
                    std::string slotName = n->name;
                    if (!currentModulePrefix_.empty())
                        slotName = currentModulePrefix_ + n->name;
                    std::string slot = slots_.count(slotName)
                        ? slots_[slotName] : "";
                    if (slot.empty())
                        slot = slots_.count(n->name) ? slots_[n->name] : "";
                    if (slot.empty())
                        throw std::runtime_error(
                            "native: variable '" + n->name + "' not declared");
                    Val v; v.ssa = "0"; v.type = VType::Int;
                    if (n->value) v = emitExpr(n->value.get());
                    line("storel " + v.ssa + ", " + slot);
                    VarInfo vi;
                    {
                        Val tmp;
                        inferFromAnnotation(n->type.get(), tmp);
                        vi.type = tmp.type;
                        vi.clsName = tmp.cls;
                        vi.elemType = tmp.elemType;
                        vi.elemClsName = tmp.elemCls;
                        vi.valType = tmp.valType;
                    }
                    if (vi.type == VType::Unknown) {
                        vi.type = v.type; vi.clsName = v.cls;
                        vi.elemType = v.elemType; vi.elemClsName = v.elemCls;
                        vi.valType = v.valType;
                    }
                    vi.tupleElemTypes = v.tupleElemTypes;
                    vi.tupleElemClsNames = v.tupleElemClsNames;
                    varInfo_[slotName] = vi;
                    return;
                }

                case StmtKind::Const: {
                    auto* n = static_cast<const ConstStmt*>(s);
                    std::string slotName = n->name;
                    if (!currentModulePrefix_.empty())
                        slotName = currentModulePrefix_ + n->name;
                    std::string slot = slots_.count(slotName)
                        ? slots_[slotName] : "";
                    if (slot.empty())
                        slot = slots_.count(n->name) ? slots_[n->name] : "";
                    if (slot.empty())
                        throw std::runtime_error(
                            "native: const '" + n->name + "' not declared");
                    Val v = emitExpr(n->value.get());
                    line("storel " + v.ssa + ", " + slot);
                    VarInfo vi;
                    vi.type = v.type; vi.clsName = v.cls;
                    vi.elemType = v.elemType; vi.elemClsName = v.elemCls;
                    vi.valType = v.valType;
                    varInfo_[slotName] = vi;
                    return;
                }

                case StmtKind::Yield: {
                    auto* n = static_cast<const YieldStmt*>(s);
                    Val v; v.ssa = "0"; v.type = VType::Int;
                    if (n->value) v = emitExpr(n->value.get());
                    line("call $vayu_gen_yield(l " + v.ssa + ")");
                    return;
                }

                case StmtKind::Class: {
                    auto* n = static_cast<const ClassStmt*>(s);
                    auto sit = statics_.find(n->name);
                    if (sit == statics_.end()) return;
                    for (auto& sf : n->staticFields) {
                        auto mit = sit->second.find(sf.name);
                        if (mit == sit->second.end()) continue;
                        const std::string& slotName = mit->second;
                        const std::string* slotPtr = nullptr;
                        {
                            auto git = slots_.find(slotName);
                            if (git != slots_.end()) slotPtr = &git->second;
                        }
                        if (!slotPtr) {
                            auto git2 = globalSlots_.find(slotName);
                            if (git2 != globalSlots_.end()) slotPtr = &git2->second;
                        }
                        if (!slotPtr)
                            throw std::runtime_error(
                                "native: static '" + n->name + "." + sf.name +
                                "' has no slot (internal)");
                        Val v; v.ssa = "0"; v.type = VType::Int;
                        if (sf.init) v = emitExpr(sf.init.get());
                        line("storel " + v.ssa + ", " + *slotPtr);
                        VarInfo vi;
                        vi.type = v.type; vi.clsName = v.cls;
                        vi.elemType = v.elemType; vi.elemClsName = v.elemCls;
                        vi.valType = v.valType;
                        varInfo_[slotName] = vi;
                    }
                    return;
                }

                case StmtKind::If:     emitIf(static_cast<const IfStmt*>(s));       return;
                case StmtKind::While:  emitWhile(static_cast<const WhileStmt*>(s)); return;
                case StmtKind::For:    emitFor(static_cast<const ForStmt*>(s));     return;
                case StmtKind::Return: emitReturn(static_cast<const ReturnStmt*>(s)); return;
                case StmtKind::Try:    emitTry(static_cast<const TryStmt*>(s));     return;
                case StmtKind::Raise:  emitRaise(static_cast<const RaiseStmt*>(s)); return;

                case StmtKind::Break:
                    if (loopStack_.empty())
                        throw std::runtime_error("'break' outside loop");
                    line("jmp " + loopStack_.back().second);
                    return;
                case StmtKind::Continue:
                    if (loopStack_.empty())
                        throw std::runtime_error("'continue' outside loop");
                    line("jmp " + loopStack_.back().first);
                    return;

                case StmtKind::Block: {
                    auto* n = static_cast<const BlockStmt*>(s);
                    emitBlock(n->body);
                    return;
                }

                case StmtKind::Extern: return;
                case StmtKind::Pass:   return;
                case StmtKind::Struct: return;
                case StmtKind::Enum:   return;
                case StmtKind::Import:
                case StmtKind::FromImport:
                    return;

                case StmtKind::Def:
                    throw std::runtime_error("native: nested 'def' not supported");
                }
            }

            void emitBlock(const Block& b) {
                for (auto& s : b.stmts) {
                    emitStmt(s.get());
                    if (terminated_) return;
                }
            }

            void emitIf(const IfStmt* n) {
                std::string cond = emitCond(n->cond.get());
                std::string lThen = newLabel("if_then_");
                std::string lElse = newLabel("if_else_");
                std::string lEnd = newLabel("if_end_");
                bool hasElse = n->elseBody.has_value() || !n->elifs.empty();

                if (hasElse) line("jnz " + cond + ", " + lThen + ", " + lElse);
                else         line("jnz " + cond + ", " + lThen + ", " + lEnd);

                raw(lThen);
                emitBlock(n->thenBody);
                if (!terminated_) line("jmp " + lEnd);

                if (hasElse) {
                    raw(lElse);
                    for (size_t i = 0; i < n->elifs.size(); ++i) {
                        auto& ec = n->elifs[i];
                        std::string ecCond = emitCond(ec.cond.get());
                        std::string ecThen = newLabel("elif_then_");
                        std::string ecElse = newLabel("elif_else_");
                        bool lastElif = (i + 1 == n->elifs.size());
                        std::string elseTarget =
                            (lastElif && !n->elseBody) ? lEnd : ecElse;
                        line("jnz " + ecCond + ", " + ecThen + ", " + elseTarget);
                        raw(ecThen);
                        emitBlock(ec.body);
                        if (!terminated_) line("jmp " + lEnd);
                        if (elseTarget == lEnd) break;
                        raw(ecElse);
                    }
                    if (n->elseBody) {
                        emitBlock(*n->elseBody);
                        if (!terminated_) line("jmp " + lEnd);
                    }
                }
                raw(lEnd);
            }

            void emitWhile(const WhileStmt* n) {
                std::string lCond = newLabel("while_cond_");
                std::string lBody = newLabel("while_body_");
                std::string lEnd = newLabel("while_end_");
                line("jmp " + lCond);
                raw(lCond);
                std::string cond = emitCond(n->cond.get());
                line("jnz " + cond + ", " + lBody + ", " + lEnd);
                raw(lBody);
                loopStack_.push_back({ lCond, lEnd });
                emitBlock(n->body);
                loopStack_.pop_back();
                if (!terminated_) line("jmp " + lCond);
                raw(lEnd);
            }

            void emitFor(const ForStmt* n) {
                if (n->iterable->kind == ExprKind::Call) {
                    auto* call = static_cast<const CallExpr*>(n->iterable.get());
                    if (call->callee->kind == ExprKind::NameRef) {
                        const auto* rn = static_cast<const NameRefExpr*>(
                            call->callee.get());
                        if (rn->name == "range") { emitForRange(n, call); return; }
                        if (generatorFunctions_.count(rn->name)) {
                            emitForGenerator(n, call);
                            return;
                        }
                    }
                }
                emitForList(n);
            }

            void emitForGenerator(const ForStmt* n, const CallExpr* /*call*/) {
                Val genVal = emitExpr(n->iterable.get());
                std::string genSlot = "%__for_gen_" + std::to_string(nextLabel_++);
                line(genSlot + " =l alloc8 8");
                line("storel " + genVal.ssa + ", " + genSlot);

                std::string savedSlot; bool hadSaved = false;
                {
                    auto it = slots_.find(n->targetName);
                    if (it != slots_.end()) { savedSlot = it->second; hadSaved = true; }
                }
                std::string uniq = std::to_string(nextLabel_++);
                std::string varSlot = "%" + mangle(n->targetName) + "_slot_" + uniq;
                slots_[n->targetName] = varSlot;
                varInfo_[n->targetName] = VarInfo{ VType::Int };
                line(varSlot + " =l alloc8 8");

                std::string lBody = newLabel("forg_body_");
                std::string lIter = newLabel("forg_iter_");
                std::string lEnd = newLabel("forg_end_");

                raw(lBody);
                {
                    std::string g = newTemp(); line(g + " =l loadl " + genSlot);
                    std::string status = newTemp();
                    line(status + " =w call $vayu_gen_try_next(l " + g +
                        ", l " + varSlot + ")");
                    line("jnz " + status + ", " + lIter + ", " + lEnd);
                }
                raw(lIter);
                loopStack_.push_back({ lBody, lEnd });
                emitBlock(n->body);
                loopStack_.pop_back();
                if (!terminated_) line("jmp " + lBody);
                raw(lEnd);

                if (hadSaved) slots_[n->targetName] = savedSlot;
                else          slots_.erase(n->targetName);
                varInfo_.erase(n->targetName);
            }

            void emitForRange(const ForStmt* n, const CallExpr* call) {
                if (call->args.size() < 1 || call->args.size() > 2)
                    throw std::runtime_error("native: range() supports 1 or 2 args");

                std::string savedSlot; bool hadSaved = false;
                {
                    auto it = slots_.find(n->targetName);
                    if (it != slots_.end()) { savedSlot = it->second; hadSaved = true; }
                }
                std::string uniq = std::to_string(nextLabel_++);
                std::string varSlot = "%" + mangle(n->targetName) + "_slot_" + uniq;
                std::string stopSlot = "%" + mangle(n->targetName) + "_stop_" + uniq;
                slots_[n->targetName] = varSlot;
                varInfo_[n->targetName] = VarInfo{ VType::Int };
                line(varSlot + " =l alloc8 8");
                line(stopSlot + " =l alloc8 8");

                if (call->args.size() == 2) {
                    Val s = emitExpr(call->args[0].value.get());
                    line("storel " + s.ssa + ", " + varSlot);
                }
                else line("storel 0, " + varSlot);
                Val stopV = emitExpr(
                    call->args[call->args.size() == 2 ? 1 : 0].value.get());
                line("storel " + stopV.ssa + ", " + stopSlot);

                std::string lBody = newLabel("for_body_");
                std::string lNext = newLabel("for_next_");
                std::string lCond = newLabel("for_cond_");
                std::string lEnd = newLabel("for_end_");
                line("jmp " + lCond);

                raw(lBody);
                loopStack_.push_back({ lNext, lEnd });
                emitBlock(n->body);
                loopStack_.pop_back();

                raw(lNext);
                {
                    std::string i = newTemp(); line(i + " =l loadl " + varSlot);
                    std::string iNew = newTemp(); line(iNew + " =l add " + i + ", 1");
                    line("storel " + iNew + ", " + varSlot);
                }
                raw(lCond);
                {
                    std::string i = newTemp(); line(i + " =l loadl " + varSlot);
                    std::string stop = newTemp(); line(stop + " =l loadl " + stopSlot);
                    std::string c = newTemp();
                    line(c + " =w csltl " + i + ", " + stop);
                    line("jnz " + c + ", " + lBody + ", " + lEnd);
                }
                raw(lEnd);

                if (hadSaved) slots_[n->targetName] = savedSlot;
                else          slots_.erase(n->targetName);
                varInfo_.erase(n->targetName);
            }

            void emitForList(const ForStmt* n) {
                Val iter = emitExpr(n->iterable.get());

                if (iter.type == VType::Map) {
                    std::string keysList = newTemp();
                    line(keysList + " =l call $vayu_map_keys(l " + iter.ssa + ")");
                    iter.ssa = keysList;
                    iter.type = VType::List;
                    iter.elemType = VType::Str;
                    iter.valType = VType::Unknown;
                }

                if (iter.type != VType::List &&
                    iter.type != VType::Tuple &&
                    iter.type != VType::Set)
                    throw std::runtime_error(
                        "native: `for` requires range(...), list, tuple, set, or map");

                std::string savedSlot; bool hadSaved = false;
                {
                    auto it = slots_.find(n->targetName);
                    if (it != slots_.end()) { savedSlot = it->second; hadSaved = true; }
                }
                std::string uniq = std::to_string(nextLabel_++);
                std::string varSlot = "%" + mangle(n->targetName) + "_slot_" + uniq;
                std::string listSlot = "%" + mangle(n->targetName) + "_lst_" + uniq;
                std::string idxSlot = "%" + mangle(n->targetName) + "_idx_" + uniq;
                std::string lenSlot = "%" + mangle(n->targetName) + "_len_" + uniq;
                slots_[n->targetName] = varSlot;

                VarInfo lv;
                lv.type = iter.elemType;
                lv.clsName = iter.elemCls;
                lv.elemType = iter.elemType;
                lv.elemClsName = iter.elemCls;
                lv.tupleElemTypes = iter.tupleElemTypes;
                lv.tupleElemClsNames = iter.tupleElemClsNames;
                varInfo_[n->targetName] = lv;

                line(varSlot + " =l alloc8 8");
                line(listSlot + " =l alloc8 8");
                line(idxSlot + " =l alloc8 8");
                line(lenSlot + " =l alloc8 8");
                line("storel " + iter.ssa + ", " + listSlot);
                line("storel 0, " + idxSlot);
                {
                    std::string len = newTemp();
                    line(len + " =l call $vayu_list_len(l " + iter.ssa + ")");
                    line("storel " + len + ", " + lenSlot);
                }

                std::string lBody = newLabel("for_body_");
                std::string lNext = newLabel("for_next_");
                std::string lCond = newLabel("for_cond_");
                std::string lEnd = newLabel("for_end_");
                line("jmp " + lCond);

                raw(lBody);
                loopStack_.push_back({ lNext, lEnd });
                {
                    std::string lst = newTemp(); line(lst + " =l loadl " + listSlot);
                    std::string idx = newTemp(); line(idx + " =l loadl " + idxSlot);
                    std::string el = newTemp();
                    line(el + " =l call $vayu_list_get(l " + lst + ", l " + idx + ")");
                    line("storel " + el + ", " + varSlot);
                }
                emitBlock(n->body);
                loopStack_.pop_back();

                raw(lNext);
                {
                    std::string idx = newTemp(); line(idx + " =l loadl " + idxSlot);
                    std::string nxt = newTemp(); line(nxt + " =l add " + idx + ", 1");
                    line("storel " + nxt + ", " + idxSlot);
                }
                raw(lCond);
                {
                    std::string idx = newTemp(); line(idx + " =l loadl " + idxSlot);
                    std::string len = newTemp(); line(len + " =l loadl " + lenSlot);
                    std::string c = newTemp();
                    line(c + " =w csltl " + idx + ", " + len);
                    line("jnz " + c + ", " + lBody + ", " + lEnd);
                }
                raw(lEnd);

                if (hadSaved) slots_[n->targetName] = savedSlot;
                else          slots_.erase(n->targetName);
                varInfo_.erase(n->targetName);
            }

            void emitReturn(const ReturnStmt* n) {
                if (n->value) {
                    Val v = emitExpr(n->value.get());
                    line("ret " + v.ssa);
                }
                else line("ret 0");
            }

            void emitRaise(const RaiseStmt* n) {
                if (n->exception) {
                    Val v = emitExpr(n->exception.get());
                    if (v.type == VType::Str) {
                        std::string typeLbl = internString("Exception");
                        std::string t = newTemp();
                        line(t + " =l call $vayu_mkexc(l " + typeLbl +
                            ", l " + v.ssa + ")");
                        v.ssa = t;
                    }
                    line("call $vayu_raise(l " + v.ssa + ")");
                }
                else {
                    line("call $vayu_reraise()");
                }
            }

            void emitTry(const TryStmt* n) {
                std::string lTry = newLabel("try_body_");
                std::string lExc = newLabel("try_exc_");
                std::string lEnd = newLabel("try_end_");

                std::string fid = newTemp();
                line(fid + " =l call $vayu_try_push()");
                std::string buf = newTemp();
                line(buf + " =l call $vayu_try_buf(l " + fid + ")");
                std::string rv = newTemp();
                line(rv + " =w call $setjmp(l " + buf + ")");
                line("jnz " + rv + ", " + lExc + ", " + lTry);

                raw(lTry);
                emitBlock(n->tryBody);
                if (!terminated_) {
                    line("call $vayu_try_pop()");
                    line("jmp " + lEnd);
                }

                raw(lExc);
                line("call $vayu_try_pop()");
                {
                    std::string et = newTemp();
                    line(et + " =l call $vayu_get_exc_type()");

                    std::vector<std::string> matchLabels;
                    std::vector<std::string> nextLabels;
                    for (size_t hi = 0; hi < n->handlers.size(); ++hi) {
                        matchLabels.push_back(newLabel("catch_match_"));
                        nextLabels.push_back(newLabel("catch_next_"));
                    }
                    std::string lReraise = newLabel("try_reraise_");

                    for (size_t i = 0; i < n->handlers.size(); ++i) {
                        if (i > 0) raw(nextLabels[i]);
                        auto& h = n->handlers[i];
                        if (h.exceptionType) {
                            std::string typeName;
                            if (h.exceptionType->kind == ExprKind::NameRef) {
                                typeName = static_cast<const NameRefExpr*>(
                                    h.exceptionType.get())->name;
                            }
                            else {
                                throw std::runtime_error(
                                    "native: exception type must be a class name");
                            }
                            std::string typeLbl = internString(typeName);

                            if (typeName == "Exception") {
                                line("jmp " + matchLabels[i]);
                            }
                            else {
                                std::string cond = newTemp();
                                line(cond + " =w call $vayu_str_eq(l " + et +
                                    ", l " + typeLbl + ")");
                                std::string nextTarget =
                                    (i + 1 < n->handlers.size())
                                    ? nextLabels[i + 1] : lReraise;
                                line("jnz " + cond + ", " + matchLabels[i] +
                                    ", " + nextTarget);
                            }
                        }
                        else {
                            line("jmp " + matchLabels[i]);
                        }
                    }

                    raw(lReraise);
                    line("call $vayu_reraise()");
                    line("jmp " + lEnd);

                    for (size_t i = 0; i < n->handlers.size(); ++i) {
                        auto& h = n->handlers[i];
                        raw(matchLabels[i]);

                        if (!h.varName.empty()) {
                            std::string excVal = newTemp();
                            line(excVal + " =l call $vayu_get_exc_value()");
                            std::string slotName = h.varName;
                            if (!slots_.count(slotName)) {
                                std::string slot = "%" + mangle(slotName) + "_slot";
                                slots_[slotName] = slot;
                                line(slot + " =l alloc8 8");
                            }
                            line("storel " + excVal + ", " + slots_[slotName]);
                            varInfo_[slotName] = VarInfo{ VType::Exc };
                        }

                        emitBlock(h.body);
                        if (!terminated_) {
                            line("jmp " + lEnd);
                        }
                    }
                }

                raw(lEnd);
                if (n->finallyBody) emitBlock(*n->finallyBody);
            }

            void emitFunction(const DefStmt* def, const ClassInfo* cls,
                const std::string& prefix) {
                std::string sym;
                if (def->isGenerator) {
                    sym = "$vayu_gen_" + (cls ? (mangle(cls->name) + "_") : prefix)
                        + mangle(def->name);
                }
                else if (cls) {
                    sym = "$vayu_mth_" + mangle(cls->name) + "_" + mangle(def->name);
                }
                else {
                    sym = "$vayu_fn_" + prefix + mangle(def->name);
                }

                std::string params;
                if (def->isGenerator) {
                    params = "l %__args_ptr";
                }
                else {
                    for (size_t i = 0; i < def->params.size(); ++i) {
                        if (i) params += ", ";
                        params += "l %p_" + mangle(def->params[i].name);
                    }
                }
                raw("function l " + sym + "(" + params + ") {");
                raw("@start");

                auto savedSlots = slots_;
                auto savedVarInfo = varInfo_;
                auto savedClass = currentClass_;
                auto savedLoop = loopStack_;
                auto savedNonEsc = nonEscapingClasses_;
                int  savedTemp = nextTemp_;
                int  savedLabel = nextLabel_;
                bool savedTerm = terminated_;
                std::string savedModPrefix = currentModulePrefix_;

                resetFunctionState();
                if (cls) currentClass_ = cls->name;
                if (!prefix.empty()) currentModulePrefix_ = prefix;

                analyzeEscapes(def->body);

                for (size_t pi = 0; pi < def->params.size(); ++pi) {
                    auto& p = def->params[pi];
                    std::string slot = "%" + mangle(p.name) + "_slot";
                    slots_[p.name] = slot;
                    line(slot + " =l alloc8 8");
                    if (def->isGenerator) {
                        std::string addr = newTemp();
                        line(addr + " =l add %__args_ptr, " +
                            std::to_string(pi * 8));
                        std::string v = newTemp();
                        line(v + " =l loadl " + addr);
                        line("storel " + v + ", " + slot);
                    }
                    else {
                        line("storel %p_" + mangle(p.name) + ", " + slot);
                    }

                    VarInfo vi;
                    if (cls && pi == 0) {
                        vi.type = VType::Obj;
                        vi.clsName = cls->name;
                    }
                    else if (p.type) {
                        Val tmp;
                        inferFromAnnotation(p.type.get(), tmp);
                        vi.type = tmp.type;
                        vi.clsName = tmp.cls;
                        vi.elemType = tmp.elemType;
                        vi.elemClsName = tmp.elemCls;
                        vi.valType = tmp.valType;

                        if (vi.type == VType::Unknown &&
                            p.type->kind == ExprKind::NameRef) {
                            const std::string& annName =
                                static_cast<const NameRefExpr*>(p.type.get())->name;
                            for (size_t ti = 0; ti < def->typeParams.size(); ++ti) {
                                if (def->typeParams[ti] != annName) continue;
                                std::string cst;
                                if (ti < def->typeParamConstraints.size())
                                    cst = def->typeParamConstraints[ti];
                                vi.type = VType::Obj;
                                if (!cst.empty() && classes_.count(cst))
                                    vi.clsName = cst;
                                break;
                            }
                        }
                    }
                    varInfo_[p.name] = vi;
                }

                std::unordered_set<std::string> names;
                collectVarsBlock(def->body, names);
                for (auto& n : names) {
                    if (slots_.count(n)) continue;
                    if (nonEscapingClasses_.count(n)) continue;
                    if (globalSlots_.count(n)) continue;
                    std::string slot = "%" + mangle(n) + "_slot";
                    slots_[n] = slot;
                    line(slot + " =l alloc8 8");
                    line("storel 0, " + slot);
                }

                for (auto& kv : nonEscapingClasses_) {
                    const std::string& varName = kv.first;
                    const std::string& className = kv.second;
                    const ClassInfo* ci = findClass(className);
                    if (!ci) continue;
                    std::string slot = "%" + mangle(varName) + "_slot";
                    std::string inst = "%" + mangle(varName) + "_inst";
                    slots_[varName] = slot;
                    line(slot + " =l alloc8 8");
                    line(inst + " =l alloc8 " + std::to_string(ci->totalSize));
                    line("storel " + inst + ", " + slot);
                    VarInfo vi;
                    vi.type = VType::Obj;
                    vi.clsName = className;
                    varInfo_[varName] = vi;
                }

                emitBlock(def->body);
                if (!terminated_) line("ret 0");
                raw("}");

                slots_ = std::move(savedSlots);
                varInfo_ = std::move(savedVarInfo);
                currentClass_ = std::move(savedClass);
                loopStack_ = std::move(savedLoop);
                nonEscapingClasses_ = std::move(savedNonEsc);
                nextTemp_ = savedTemp;
                nextLabel_ = savedLabel;
                terminated_ = savedTerm;
                currentModulePrefix_ = std::move(savedModPrefix);
            }

            void emitClassCtor(const ClassInfo& ci) {
                std::string sym = "$vayu_ctor_" + mangle(ci.name);
                const DefStmt* userInit = ci.initDecl;

                std::vector<std::string> paramNames;
                if (userInit) {
                    for (size_t i = 1; i < userInit->params.size(); ++i)
                        paramNames.push_back(userInit->params[i].name);
                }
                else {
                    for (auto& f : ci.fields) paramNames.push_back(f);
                }

                std::string params;
                for (size_t i = 0; i < paramNames.size(); ++i) {
                    if (i) params += ", ";
                    params += "l %p_" + mangle(paramNames[i]);
                }
                raw("function l " + sym + "(" + params + ") {");
                raw("@start");

                auto savedSlots = slots_;
                auto savedVarInfo = varInfo_;
                auto savedClass = currentClass_;
                auto savedLoop = loopStack_;
                auto savedNonEsc = nonEscapingClasses_;
                int  savedTemp = nextTemp_;
                int  savedLabel = nextLabel_;
                bool savedTerm = terminated_;
                std::string savedModPrefix = currentModulePrefix_;

                resetFunctionState();
                currentClass_ = ci.name;

                std::string self = newTemp();
                line(self + " =l call $vayu_alloc(l " +
                    std::to_string(ci.totalSize) + ")");

                if (userInit && ci.initOwner) {
                    std::string argList = "l " + self;
                    for (auto& p : paramNames)
                        argList += ", l %p_" + mangle(p);
                    std::string initSym = "$vayu_mth_" + mangle(ci.initOwner->name) +
                        "_" + mangle("__init__");
                    line("call " + initSym + "(" + argList + ")");
                }
                else {
                    for (size_t i = 0; i < ci.fields.size(); ++i) {
                        const std::string& f = ci.fields[i];
                        int off = ci.fieldOffsets.at(f);
                        std::string addr = newTemp();
                        line(addr + " =l add " + self + ", " +
                            std::to_string(off));
                        line("storel %p_" + mangle(f) + ", " + addr);
                    }
                }
                line("ret " + self);
                raw("}");

                slots_ = std::move(savedSlots);
                varInfo_ = std::move(savedVarInfo);
                currentClass_ = std::move(savedClass);
                loopStack_ = std::move(savedLoop);
                nonEscapingClasses_ = std::move(savedNonEsc);
                nextTemp_ = savedTemp;
                nextLabel_ = savedLabel;
                terminated_ = savedTerm;
                currentModulePrefix_ = std::move(savedModPrefix);
            }

            std::string emitCond(const Expr* e) {
                if (!e) return "0";

                if (e->kind == ExprKind::Binary) {
                    auto* b = static_cast<const BinaryExpr*>(e);

                    switch (b->op) {
                    case BinOp::Eq: case BinOp::NotEq:
                    case BinOp::Lt: case BinOp::Gt:
                    case BinOp::LtEq: case BinOp::GtEq: {
                        Val a = emitExpr(b->lhs.get());
                        Val c = emitExpr(b->rhs.get());

                        bool strCmp =
                            (a.type == VType::Str || c.type == VType::Str) &&
                            (b->op == BinOp::Eq || b->op == BinOp::NotEq);
                        if (strCmp) {
                            const char* fn = (b->op == BinOp::Eq)
                                ? "$vayu_str_eq" : "$vayu_str_ne";
                            std::string t = newTemp();
                            line(t + " =l call " + fn +
                                "(l " + a.ssa + ", l " + c.ssa + ")");
                            std::string w = newTemp();
                            line(w + " =w cnel " + t + ", 0");
                            return w;
                        }

                        /* Phase 24.0 - float compare. */
                        bool anyF =
                            (a.type == VType::Float || c.type == VType::Float);
                        if (anyF) {
                            std::string aD;
                            if (a.type == VType::Float) {
                                aD = newTemp();
                                line(aD + " =d cast " + a.ssa);
                            }
                            else {
                                aD = newTemp();
                                line(aD + " =d sltof " + a.ssa);
                            }
                            std::string cD;
                            if (c.type == VType::Float) {
                                cD = newTemp();
                                line(cD + " =d cast " + c.ssa);
                            }
                            else {
                                cD = newTemp();
                                line(cD + " =d sltof " + c.ssa);
                            }
                            if (b->op == BinOp::NotEq) {
                                std::string cw = newTemp();
                                line(cw + " =w ceqd " + aD + ", " + cD);
                                std::string inv = newTemp();
                                line(inv + " =w xor " + cw + ", 1");
                                return inv;
                            }
                            const char* cop = "ceqd";
                            if (b->op == BinOp::Lt)   cop = "cltd";
                            else if (b->op == BinOp::Gt)   cop = "cgtd";
                            else if (b->op == BinOp::LtEq) cop = "cled";
                            else if (b->op == BinOp::GtEq) cop = "cged";
                            std::string w = newTemp();
                            line(w + " =w " + std::string(cop) + " " + aD + ", " + cD);
                            return w;
                        }

                        const char* opName = nullptr;
                        switch (b->op) {
                        case BinOp::Eq:    opName = "ceql";  break;
                        case BinOp::NotEq: opName = "cnel";  break;
                        case BinOp::Lt:    opName = "csltl"; break;
                        case BinOp::Gt:    opName = "csgtl"; break;
                        case BinOp::LtEq:  opName = "cslel"; break;
                        case BinOp::GtEq:  opName = "csgel"; break;
                        default: break;
                        }
                        std::string w = newTemp();
                        line(w + " =w " + opName + " " + a.ssa + ", " + c.ssa);
                        return w;
                    }

                    case BinOp::And: {
                        std::string w1 = emitCond(b->lhs.get());
                        std::string slot = newTemp();
                        line(slot + " =l alloc8 8");
                        std::string lShort = newLabel("andc_short_");
                        std::string lFall = newLabel("andc_fall_");
                        std::string lEnd = newLabel("andc_end_");
                        line("jnz " + w1 + ", " + lFall + ", " + lShort);
                        raw(lShort);
                        line("storel 0, " + slot);
                        line("jmp " + lEnd);
                        raw(lFall);
                        std::string w2 = emitCond(b->rhs.get());
                        std::string e2 = newTemp();
                        line(e2 + " =l extsw " + w2);
                        line("storel " + e2 + ", " + slot);
                        raw(lEnd);
                        std::string res = newTemp();
                        line(res + " =l loadl " + slot);
                        std::string rw = newTemp();
                        line(rw + " =w copy " + res);
                        return rw;
                    }
                    case BinOp::Or: {
                        std::string w1 = emitCond(b->lhs.get());
                        std::string slot = newTemp();
                        line(slot + " =l alloc8 8");
                        std::string lShort = newLabel("orc_short_");
                        std::string lFall = newLabel("orc_fall_");
                        std::string lEnd = newLabel("orc_end_");
                        line("jnz " + w1 + ", " + lShort + ", " + lFall);
                        raw(lShort);
                        line("storel 1, " + slot);
                        line("jmp " + lEnd);
                        raw(lFall);
                        std::string w2 = emitCond(b->rhs.get());
                        std::string e2 = newTemp();
                        line(e2 + " =l extsw " + w2);
                        line("storel " + e2 + ", " + slot);
                        raw(lEnd);
                        std::string res = newTemp();
                        line(res + " =l loadl " + slot);
                        std::string rw = newTemp();
                        line(rw + " =w copy " + res);
                        return rw;
                    }
                    default: break;
                    }
                }

                if (e->kind == ExprKind::Unary) {
                    auto* u = static_cast<const UnaryExpr*>(e);
                    if (u->op == UnOp::Not) {
                        std::string w = emitCond(u->operand.get());
                        std::string inv = newTemp();
                        line(inv + " =w xor " + w + ", 1");
                        return inv;
                    }
                }

                if (e->kind == ExprKind::BoolLit) {
                    auto* bl = static_cast<const BoolLitExpr*>(e);
                    return bl->value ? "1" : "0";
                }

                Val v = emitExpr(e);
                std::string t = newTemp();
                line(t + " =w cnel " + v.ssa + ", 0");
                return t;
            }
        };

        void QbeEmitter::collectVarsStmt(const Stmt* s,
            std::unordered_set<std::string>& out) {
            if (!s) return;
            switch (s->kind) {
            case StmtKind::Assign: {
                auto* n = static_cast<const AssignStmt*>(s);
                if (n->target->kind == ExprKind::NameRef)
                    out.insert(static_cast<const NameRefExpr*>(
                        n->target.get())->name);
                break;
            }
            case StmtKind::AnnotAssign: {
                auto* n = static_cast<const AnnotAssignStmt*>(s);
                out.insert(n->name);
                break;
            }
            case StmtKind::Const: {
                auto* n = static_cast<const ConstStmt*>(s);
                out.insert(n->name);
                break;
            }
            case StmtKind::If: {
                auto* n = static_cast<const IfStmt*>(s);
                collectVarsBlock(n->thenBody, out);
                for (auto& ec : n->elifs) collectVarsBlock(ec.body, out);
                if (n->elseBody) collectVarsBlock(*n->elseBody, out);
                break;
            }
            case StmtKind::While: {
                auto* n = static_cast<const WhileStmt*>(s);
                collectVarsBlock(n->body, out);
                break;
            }
            case StmtKind::For: {
                auto* n = static_cast<const ForStmt*>(s);
                collectVarsBlock(n->body, out);
                break;
            }
            case StmtKind::Try: {
                auto* n = static_cast<const TryStmt*>(s);
                collectVarsBlock(n->tryBody, out);
                for (auto& h : n->handlers) {
                    if (!h.varName.empty()) out.insert(h.varName);
                    collectVarsBlock(h.body, out);
                }
                if (n->finallyBody) collectVarsBlock(*n->finallyBody, out);
                break;
            }
            case StmtKind::Block: {
                auto* n = static_cast<const BlockStmt*>(s);
                collectVarsBlock(n->body, out);
                break;
            }
            case StmtKind::Extern: break;
            default: break;
            }
        }
        void QbeEmitter::collectVarsBlock(const Block& b,
            std::unordered_set<std::string>& out) {
            for (auto& s : b.stmts) collectVarsStmt(s.get(), out);
        }

    } // anonymous namespace

    static const char* kRuntimeC = R"C(
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <direct.h>
#  include <windows.h>
#  include <commctrl.h>
#  include <bcrypt.h>
#  include <gdiplus.h>
#  include <wincodec.h>
#  include <objbase.h>
#  include <d3d11.h>
#  include <d3dcompiler.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#endif

typedef struct { int64_t len; char data[]; } VayuStr;
typedef struct {
    int64_t len;
    int64_t cap;
    int64_t* items;
    int8_t*  tags;   /* per-element kind: 0=int 1=bool 2=str 3=list 4=map 5=tuple 6=set */
} VayuList;
typedef struct { VayuStr* key; int64_t value; uint8_t used; } VayuMapEntry;
typedef struct { int64_t len; int64_t cap; VayuMapEntry* entries; } VayuMap;
typedef struct { VayuStr* typeName; VayuStr* message; } VayuExc;

void vayu_raise_str(VayuStr* typeName, VayuStr* msg);
int64_t vayu_str_to_int(VayuStr* s);

/* Forward declarations for list helpers used by the new string methods
   (partition, splitlines, rsplit) that are defined before the list block. */
VayuList* vayu_list_new(void);
void vayu_list_push(VayuList* l, int64_t v);
void vayu_list_push_tagged(VayuList* l, int64_t v, int64_t tag);

/* Phase 15.2d: forward decls for vayu_map_slot, which is defined before
   the map implementation block. */
static VayuStr* vayu_concat_c(const char* prefix, VayuStr* s);
VayuMap* vayu_map_new(void);
void     vayu_map_put(VayuMap* m, VayuStr* k, int64_t v);
static uint64_t hash_str(VayuStr* s);
static VayuMapEntry* map_find(VayuMap* m, VayuStr* k);
static void map_grow(VayuMap* m);

static int    g_argc = 0;
static char** g_argv = NULL;

void* vayu_alloc(int64_t size) {
    void* p = malloc((size_t)size);
    if (!p) { fprintf(stderr, "vayu: oom\n"); exit(1); }
    return p;
}

static VayuStr* vayu_mkstr(const char* cstr, int64_t n) {
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)n + 1);
    s->len = n;
    if (n > 0) memcpy(s->data, cstr, (size_t)n);
    s->data[n] = 0;
    return s;
}
static VayuStr* vayu_mkstr_c(const char* cstr) {
    return vayu_mkstr(cstr, (int64_t)strlen(cstr));
}

void vayu_print_int(long long v) { printf("%lld\n", v); }
void vayu_print_bool(long long v) { printf("%s\n", v ? "true" : "false"); }
void vayu_print_int_noln(long long v) { printf("%lld", v); }
void vayu_print_bool_noln(long long v) { printf("%s", v ? "true" : "false"); }
void vayu_print_space(void) { putchar(' '); }
void vayu_print_ln(void) { putchar('\n'); }
void vayu_print_str(VayuStr* s) {
    fwrite(s->data, 1, (size_t)s->len, stdout); putchar('\n');
}
void vayu_print_str_noln(VayuStr* s) {
    fwrite(s->data, 1, (size_t)s->len, stdout);
}

/* ---- Phase 24.0 - float helpers ---- */

static double vayu_bits_to_double(int64_t bits) {
    double d; memcpy(&d, &bits, 8); return d;
}
static int64_t vayu_double_to_bits(double d) {
    int64_t bits; memcpy(&bits, &d, 8); return bits;
}

static void vayu_fmt_double(char* buf, size_t cap, double d) {
    if (isnan(d)) { snprintf(buf, cap, "nan"); return; }
    if (isinf(d)) { snprintf(buf, cap, d < 0 ? "-inf" : "inf"); return; }
    snprintf(buf, cap, "%.15g", d);
    int hasDot = 0;
    for (size_t i = 0; buf[i]; ++i)
        if (buf[i] == '.' || buf[i] == 'e') { hasDot = 1; break; }
    if (!hasDot) {
        size_t n = strlen(buf);
        if (n + 2 < cap) { buf[n] = '.'; buf[n + 1] = '0'; buf[n + 2] = 0; }
    }
}

void vayu_print_float_noln(int64_t bits) {
    char buf[64];
    vayu_fmt_double(buf, sizeof(buf), vayu_bits_to_double(bits));
    fputs(buf, stdout);
}

VayuStr* vayu_float_to_str(int64_t bits) {
    char buf[64];
    vayu_fmt_double(buf, sizeof(buf), vayu_bits_to_double(bits));
    return vayu_mkstr_c(buf);
}

int64_t vayu_floordiv_d(int64_t a_bits, int64_t b_bits) {
    double a = vayu_bits_to_double(a_bits);
    double b = vayu_bits_to_double(b_bits);
    if (b == 0.0) {
        vayu_raise_str(vayu_mkstr_c("ZeroDivisionError"),
                       vayu_mkstr_c("division by zero"));
    }
    return vayu_double_to_bits(floor(a / b));
}

int64_t vayu_mod_d(int64_t a_bits, int64_t b_bits) {
    double a = vayu_bits_to_double(a_bits);
    double b = vayu_bits_to_double(b_bits);
    if (b == 0.0) {
        vayu_raise_str(vayu_mkstr_c("ZeroDivisionError"),
                       vayu_mkstr_c("modulo by zero"));
    }
    double m = fmod(a, b);
    if (m != 0.0 && ((m < 0.0) != (b < 0.0))) m += b;
    return vayu_double_to_bits(m);
}

int64_t vayu_pow_d(int64_t a_bits, int64_t b_bits) {
    double a = vayu_bits_to_double(a_bits);
    double b = vayu_bits_to_double(b_bits);
    return vayu_double_to_bits(pow(a, b));
}

int64_t vayu_str_to_float(int64_t s_sp) {
    VayuStr* s = (VayuStr*)s_sp;
    char buf[256];
    int64_t n = s->len < 255 ? s->len : 255;
    memcpy(buf, s->data, (size_t)n);
    buf[n] = 0;
    return vayu_double_to_bits(strtod(buf, NULL));
}

/* ---- Phase 24.1 - native math module ---- */

#define VAYU_MATH_UNARY(name, fn) \
    int64_t vayu_math_##name(int64_t a) { \
        return vayu_double_to_bits(fn(vayu_bits_to_double(a))); \
    }
VAYU_MATH_UNARY(sqrt, sqrt)
VAYU_MATH_UNARY(sin,  sin)
VAYU_MATH_UNARY(cos,  cos)
VAYU_MATH_UNARY(tan,  tan)
VAYU_MATH_UNARY(asin, asin)
VAYU_MATH_UNARY(acos, acos)
VAYU_MATH_UNARY(atan, atan)
VAYU_MATH_UNARY(sinh, sinh)
VAYU_MATH_UNARY(cosh, cosh)
VAYU_MATH_UNARY(tanh, tanh)
VAYU_MATH_UNARY(asinh, asinh)
VAYU_MATH_UNARY(acosh, acosh)
VAYU_MATH_UNARY(atanh, atanh)
VAYU_MATH_UNARY(exp,  exp)
VAYU_MATH_UNARY(log,  log)
VAYU_MATH_UNARY(log2, log2)
VAYU_MATH_UNARY(log10, log10)
/* floor / ceil / trunc: the plain variants return a double bit pattern
   (matching the VAYU_MATH_UNARY convention).  The emitter converts the
   result to int64 with dtosi when the tree-walk semantics require an
   Int result.  The `_i` variants are kept for any caller that wants
   the conversion done inside the runtime. */
int64_t vayu_math_floor(int64_t a) { return vayu_double_to_bits(floor(vayu_bits_to_double(a))); }
int64_t vayu_math_ceil (int64_t a) { return vayu_double_to_bits(ceil (vayu_bits_to_double(a))); }
int64_t vayu_math_trunc(int64_t a) { return vayu_double_to_bits(trunc(vayu_bits_to_double(a))); }
int64_t vayu_math_floor_i(int64_t a) { return (int64_t)floor(vayu_bits_to_double(a)); }
int64_t vayu_math_ceil_i (int64_t a) { return (int64_t)ceil (vayu_bits_to_double(a)); }
int64_t vayu_math_trunc_i(int64_t a) { return (int64_t)trunc(vayu_bits_to_double(a)); }
VAYU_MATH_UNARY(fabs, fabs)
VAYU_MATH_UNARY(cbrt, cbrt)
VAYU_MATH_UNARY(expm1, expm1)
VAYU_MATH_UNARY(log1p, log1p)
VAYU_MATH_UNARY(tgamma, tgamma)
VAYU_MATH_UNARY(lgamma, lgamma)
VAYU_MATH_UNARY(erf,  erf)
VAYU_MATH_UNARY(erfc, erfc)
#undef VAYU_MATH_UNARY

#define VAYU_MATH_BINARY(name, fn) \
    int64_t vayu_math_##name(int64_t a, int64_t b) { \
        return vayu_double_to_bits(fn(vayu_bits_to_double(a), vayu_bits_to_double(b))); \
    }
VAYU_MATH_BINARY(pow, pow)
VAYU_MATH_BINARY(atan2, atan2)
VAYU_MATH_BINARY(hypot, hypot)
VAYU_MATH_BINARY(fmod, fmod)
VAYU_MATH_BINARY(copysign, copysign)
#undef VAYU_MATH_BINARY

int64_t vayu_math_degrees(int64_t a) {
    return vayu_double_to_bits(vayu_bits_to_double(a) * 180.0 / 3.14159265358979323846);
}
int64_t vayu_math_radians(int64_t a) {
    return vayu_double_to_bits(vayu_bits_to_double(a) * 3.14159265358979323846 / 180.0);
}
int64_t vayu_math_is_nan(int64_t a)    { return isnan(vayu_bits_to_double(a)) ? 1 : 0; }
int64_t vayu_math_is_inf(int64_t a)    { return isinf(vayu_bits_to_double(a)) ? 1 : 0; }
int64_t vayu_math_is_finite(int64_t a) { return isfinite(vayu_bits_to_double(a)) ? 1 : 0; }

void vayu_print_raw(VayuStr* s) {
    fwrite(s->data, 1, (size_t)s->len, stdout);
    fflush(stdout);
}

/* ---- Phase 15.1: C FFI string bridging ---- */
char* vayu_str_cstr(VayuStr* s) { return s->data; }
VayuStr* vayu_cstr_to_str(const char* p) {
    if (!p) return vayu_mkstr("", 0);
    return vayu_mkstr_c(p);
}

/* ===========================================================================
 * Phase 16.0–16.2: CPython dynamic-loading shim.
 *
 *  16.0  dynamic discovery + py.init / py.version / py.run
 *  16.1  value bridging: py.int / py.str / py.bool + py.from_*
 *  16.2  calling:        py.import / py.getattr / py.call / py.decref
 *
 * Loads python3.dll (or libpython3.so) at first use via:
 *   1) VAYU_PYTHON_DLL / VAYU_PYTHON_SO env override
 *   2) auto-discovery by shelling out to `where python`
 *   3) bare-name candidates on PATH
 * ========================================================================= */
#ifdef _WIN32
#  define VAYU_PY_DLL_T HMODULE
#  define VAYU_PY_OPEN(n)  LoadLibraryA(n)
#  define VAYU_PY_SYM(h,n) GetProcAddress(h,n)
#else
#  include <dlfcn.h>
#  define VAYU_PY_DLL_T void*
#  define VAYU_PY_OPEN(n)  dlopen(n, RTLD_LAZY | RTLD_GLOBAL)
#  define VAYU_PY_SYM(h,n) dlsym(h,n)
#endif

static VAYU_PY_DLL_T g_py_dll = NULL;
static int           g_py_inited = 0;

typedef int         (*vayu_py_init_t)(int);
typedef int         (*vayu_py_final_t)(void);
typedef int         (*vayu_py_isinit_t)(void);
typedef const char* (*vayu_py_ver_t)(void);
typedef int         (*vayu_py_runstr_t)(const char*);
typedef void        (*vayu_py_errprint_t)(void);

typedef int64_t     (*vayu_py_long_from_t)(int64_t);
typedef int64_t     (*vayu_py_long_as_t)(int64_t);
typedef int64_t     (*vayu_py_unicode_from_t)(const char*, int64_t);
typedef const char* (*vayu_py_unicode_as_t)(int64_t);
typedef int64_t     (*vayu_py_bool_from_t)(int64_t);
typedef int         (*vayu_py_is_true_t)(int64_t);

typedef int64_t     (*vayu_py_import_t)(const char*);
typedef int64_t     (*vayu_py_getattr_t)(int64_t, const char*);
typedef int64_t     (*vayu_py_call_t)(int64_t, int64_t, int64_t);
typedef int64_t     (*vayu_py_tuple_new_t)(int64_t);
typedef int         (*vayu_py_tuple_set_t)(int64_t, int64_t, int64_t);
typedef void        (*vayu_py_decref_t)(int64_t);
typedef void        (*vayu_py_incref_t)(int64_t);

static vayu_py_init_t      p_Py_InitializeEx           = NULL;
static vayu_py_final_t     p_Py_FinalizeEx             = NULL;
static vayu_py_isinit_t    p_Py_IsInitialized          = NULL;
static vayu_py_ver_t       p_Py_GetVersion             = NULL;
static vayu_py_runstr_t    p_PyRun_SimpleString        = NULL;
static vayu_py_errprint_t  p_PyErr_Print               = NULL;

static vayu_py_long_from_t    p_PyLong_FromLongLong         = NULL;
static vayu_py_long_as_t      p_PyLong_AsLongLong           = NULL;
static vayu_py_unicode_from_t p_PyUnicode_FromStringAndSize = NULL;
static vayu_py_unicode_as_t   p_PyUnicode_AsUTF8            = NULL;
static vayu_py_bool_from_t    p_PyBool_FromLong             = NULL;
static vayu_py_is_true_t      p_PyObject_IsTrue             = NULL;

static vayu_py_import_t     p_PyImport_ImportModule    = NULL;
static vayu_py_getattr_t    p_PyObject_GetAttrString   = NULL;
static vayu_py_call_t       p_PyObject_Call            = NULL;
static vayu_py_tuple_new_t  p_PyTuple_New              = NULL;
static vayu_py_tuple_set_t  p_PyTuple_SetItem          = NULL;
static vayu_py_decref_t     p_Py_DecRef                = NULL;
static vayu_py_incref_t     p_Py_IncRef                = NULL;

/* ---- DLL discovery ---- */

/* Upgrade the loader: use LOAD_WITH_ALTERED_SEARCH_PATH so that when we
   load tools\python313.dll, its vcruntime*.dll and DLLs\*.pyd siblings
   are resolved from tools\ first (not from the exe's directory). */
#ifdef _WIN32
#  undef VAYU_PY_OPEN
static VAYU_PY_DLL_T vayu_py_open_ex(const char* n) {
    if (strchr(n, '\\') || strchr(n, '/'))
        return LoadLibraryExA(n, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    return LoadLibraryA(n);
}
#  define VAYU_PY_OPEN(n)  vayu_py_open_ex(n)
#endif

/* Versioned runtime DLLs only.  We deliberately exclude the stable-ABI
   forwarder `python3.dll` — on this system it does not export
   PyRun_SimpleString, and using it silently loses py.run(). */
#ifdef _WIN32
static const char* const vayu_py_dll_names[] = {
    "python313.dll", "python312.dll", "python311.dll",
    "python310.dll", "python39.dll",  "python38.dll",
    "python37.dll",  "python36.dll",
    NULL
};
#else
static const char* const vayu_py_dll_names[] = {
    "libpython3.13.so", "libpython3.12.so", "libpython3.11.so",
    "libpython3.10.so", "libpython3.9.so",  "libpython3.8.so",
    "libpython3.so",
    NULL
};
#endif

static int vayu_py_try_dir(const char* dir, char* out, size_t outsz) {
    if (!dir || !*dir) return 0;
    for (int i = 0; vayu_py_dll_names[i]; ++i) {
        int n = snprintf(out, outsz,
#ifdef _WIN32
                         "%s\\%s",
#else
                         "%s/%s",
#endif
                         dir, vayu_py_dll_names[i]);
        if (n <= 0 || (size_t)n >= outsz) continue;
        FILE* g = fopen(out, "rb");
        if (!g) continue;
        fclose(g);
        return 1;
    }
    return 0;
}

/* Search <cwd>\tools\python3XX.dll and walk up a few levels, so an exe
   run from build\x64-debug\bin still finds the DLLs the user dropped in
   E:\Vayu\tools\. */
static int vayu_py_search_upward(char* out, size_t outsz, int maxUp) {
    char buf[2048];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)) == NULL) return 0;
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return 0;
#endif
    for (int i = 0; i <= maxUp; ++i) {
        char cand[2048];
#ifdef _WIN32
        snprintf(cand, sizeof(cand), "%s\\tools", buf);
#else
        snprintf(cand, sizeof(cand), "%s/tools", buf);
#endif
        if (vayu_py_try_dir(cand, out, outsz)) return 1;
        if (vayu_py_try_dir(buf, out, outsz)) return 1;

        size_t blen = strlen(buf);
        while (blen > 0 && (buf[blen-1] == '\\' || buf[blen-1] == '/'))
            buf[--blen] = 0;
#ifdef _WIN32
        char* slash = strrchr(buf, '\\');
#else
        char* slash = strrchr(buf, '/');
#endif
        if (!slash) break;
        if (slash == buf) { buf[1] = 0; }
        else { *slash = 0; }
    }
    return 0;
}

static int vayu_py_discover(char* out, size_t outsz) {
#ifdef _WIN32
    FILE* f = _popen("where python 2>nul", "r");
#else
    FILE* f = popen("which python3 2>/dev/null", "r");
#endif
    if (!f) return 0;
    char line[2048];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        size_t start = 0;
        while (start < n && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start > 0) { memmove(line, line + start, n - start + 1); n -= start; }
        if (n < 2) continue;
#ifdef _WIN32
        char* slash = strrchr(line, '\\');
#else
        char* slash = strrchr(line, '/');
#endif
        if (!slash) continue;
        *slash = 0;
        if (vayu_py_try_dir(line, out, outsz)) { found = 1; break; }
    }
#ifdef _WIN32
    _pclose(f);
#else
    pclose(f);
#endif
    return found;
}

static int vayu_py_load_dll(void) {
    if (g_py_dll) return 1;
    const int dbg = (getenv("VAYU_PY_DEBUG") != NULL);

    /* 1) Env override. */
    const char* envp =
#ifdef _WIN32
        getenv("VAYU_PYTHON_DLL");
#else
        getenv("VAYU_PYTHON_SO");
#endif
    if (envp && *envp) {
        if (dbg) fprintf(stderr, "[py] env override: %s\n", envp);
        g_py_dll = VAYU_PY_OPEN(envp);
        if (dbg && !g_py_dll)
            fprintf(stderr, "[py] env override load failed\n");
    }

    /* 2) tools\python3XX.dll next to or above the cwd. */
    if (!g_py_dll) {
        char local[2048];
        if (vayu_py_search_upward(local, sizeof(local), 6)) {
            if (dbg) fprintf(stderr, "[py] upward search: %s\n", local);
            g_py_dll = VAYU_PY_OPEN(local);
            if (dbg && !g_py_dll)
                fprintf(stderr, "[py] LoadLibrary failed on %s\n", local);
        } else if (dbg) {
            fprintf(stderr, "[py] upward search found nothing "
                            "(looked for tools\\python3XX.dll)\n");
        }
    }

    /* 3) `where python` discovery — parent dir of python.exe. */
    if (!g_py_dll) {
        char discovered[2048];
        if (vayu_py_discover(discovered, sizeof(discovered))) {
            if (dbg) fprintf(stderr, "[py] where python: %s\n", discovered);
            g_py_dll = VAYU_PY_OPEN(discovered);
            if (dbg && !g_py_dll)
                fprintf(stderr, "[py] LoadLibrary failed on %s\n", discovered);
        } else if (dbg) {
            fprintf(stderr, "[py] `where python` discovery failed\n");
        }
    }

    /* 4) Bare-name candidates — let the OS search PATH. */
    if (!g_py_dll) {
        for (int i = 0; vayu_py_dll_names[i]; ++i) {
            g_py_dll = VAYU_PY_OPEN(vayu_py_dll_names[i]);
            if (g_py_dll) {
                if (dbg) fprintf(stderr, "[py] bare-name load: %s\n",
                                 vayu_py_dll_names[i]);
                break;
            }
        }
    }
    if (!g_py_dll) {
        if (dbg) fprintf(stderr, "[py] all load paths failed\n");
        return 0;
    }

    if (dbg) {
        char modpath[2048];
        DWORD got = GetModuleFileNameA(g_py_dll, modpath, sizeof(modpath));
        if (got > 0) modpath[got] = 0;
        else         modpath[0] = 0;
        fprintf(stderr, "[py] loaded: %s\n", modpath);
    }

    p_Py_InitializeEx    = (vayu_py_init_t)   VAYU_PY_SYM(g_py_dll, "Py_InitializeEx");
    p_Py_FinalizeEx      = (vayu_py_final_t)  VAYU_PY_SYM(g_py_dll, "Py_FinalizeEx");
    p_Py_IsInitialized   = (vayu_py_isinit_t) VAYU_PY_SYM(g_py_dll, "Py_IsInitialized");
    p_Py_GetVersion      = (vayu_py_ver_t)    VAYU_PY_SYM(g_py_dll, "Py_GetVersion");
    p_PyRun_SimpleString = (vayu_py_runstr_t) VAYU_PY_SYM(g_py_dll, "PyRun_SimpleString");
    p_PyErr_Print        = (vayu_py_errprint_t)VAYU_PY_SYM(g_py_dll, "PyErr_Print");

    p_PyLong_FromLongLong         = (vayu_py_long_from_t)
        VAYU_PY_SYM(g_py_dll, "PyLong_FromLongLong");
    p_PyLong_AsLongLong           = (vayu_py_long_as_t)
        VAYU_PY_SYM(g_py_dll, "PyLong_AsLongLong");
    p_PyUnicode_FromStringAndSize = (vayu_py_unicode_from_t)
        VAYU_PY_SYM(g_py_dll, "PyUnicode_FromStringAndSize");
    p_PyUnicode_AsUTF8            = (vayu_py_unicode_as_t)
        VAYU_PY_SYM(g_py_dll, "PyUnicode_AsUTF8");
    p_PyBool_FromLong             = (vayu_py_bool_from_t)
        VAYU_PY_SYM(g_py_dll, "PyBool_FromLong");
    p_PyObject_IsTrue             = (vayu_py_is_true_t)
        VAYU_PY_SYM(g_py_dll, "PyObject_IsTrue");

    p_PyImport_ImportModule  = (vayu_py_import_t)
        VAYU_PY_SYM(g_py_dll, "PyImport_ImportModule");
    p_PyObject_GetAttrString = (vayu_py_getattr_t)
        VAYU_PY_SYM(g_py_dll, "PyObject_GetAttrString");
    p_PyObject_Call          = (vayu_py_call_t)
        VAYU_PY_SYM(g_py_dll, "PyObject_Call");
    p_PyTuple_New            = (vayu_py_tuple_new_t)
        VAYU_PY_SYM(g_py_dll, "PyTuple_New");
    p_PyTuple_SetItem        = (vayu_py_tuple_set_t)
        VAYU_PY_SYM(g_py_dll, "PyTuple_SetItem");
    p_Py_DecRef              = (vayu_py_decref_t)
        VAYU_PY_SYM(g_py_dll, "Py_DecRef");
    p_Py_IncRef              = (vayu_py_incref_t)
        VAYU_PY_SYM(g_py_dll, "Py_IncRef");

    if (!p_Py_InitializeEx || !p_Py_GetVersion || !p_PyRun_SimpleString) {
        if (dbg) fprintf(stderr,
            "[py] required symbols missing after load: "
            "InitializeEx=%p GetVersion=%p Run_SimpleString=%p\n",
            (void*)p_Py_InitializeEx, (void*)p_Py_GetVersion,
            (void*)p_PyRun_SimpleString);
        g_py_dll = NULL;
        return 0;
    }
    return 1;
}

/* ---- 16.0: init / version / run ---- */
int64_t vayu_py_init(void) {
    if (g_py_inited) return 1;
    if (!vayu_py_load_dll()) return 0;
    p_Py_InitializeEx(0);
    g_py_inited = 1;
    return 1;
}

VayuStr* vayu_py_version(void) {
    if (!vayu_py_init()) return vayu_mkstr_c("(no python runtime found)");
    const char* v = p_Py_GetVersion();
    if (!v) return vayu_mkstr_c("(unknown)");
    const char* nl = strchr(v, '\n');
    if (nl) return vayu_mkstr(v, (int64_t)(nl - v));
    return vayu_mkstr_c(v);
}

void vayu_py_run(const char* src) {
    if (!vayu_py_init()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    int rc = p_PyRun_SimpleString(src);
    if (rc != 0 && p_PyErr_Print) p_PyErr_Print();
}

/* ---- 16.1: value bridging ---- */
int64_t vayu_py_int(int64_t v) {
    if (!vayu_py_init() || !p_PyLong_FromLongLong) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyLong_FromLongLong(v);
}

int64_t vayu_py_bool(int64_t v) {
    if (!vayu_py_init() || !p_PyBool_FromLong) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyBool_FromLong(v ? 1 : 0);
}

int64_t vayu_py_str(int64_t sp) {
    if (!vayu_py_init() || !p_PyUnicode_FromStringAndSize) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* s = (VayuStr*)sp;
    return p_PyUnicode_FromStringAndSize(s->data, s->len);
}

int64_t vayu_py_from_int(int64_t p) {
    if (!vayu_py_init() || !p_PyLong_AsLongLong) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyLong_AsLongLong(p);
}

int64_t vayu_py_from_str(int64_t p) {
    if (!vayu_py_init() || !p_PyUnicode_AsUTF8) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    const char* s = p_PyUnicode_AsUTF8(p);
    if (!s) return (int64_t)vayu_mkstr("", 0);
    return (int64_t)vayu_mkstr_c(s);
}

int64_t vayu_py_from_bool(int64_t p) {
    if (!vayu_py_init() || !p_PyObject_IsTrue) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return (int64_t)(p_PyObject_IsTrue(p) ? 1 : 0);
}

/* ---- 16.2: calling Python ---- */
int64_t vayu_py_import(const char* name) {
    if (!vayu_py_init() || !p_PyImport_ImportModule) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyImport_ImportModule(name);
}

int64_t vayu_py_getattr(int64_t obj, const char* name) {
    if (!vayu_py_init() || !p_PyObject_GetAttrString) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!obj) return 0;
    return p_PyObject_GetAttrString(obj, name);
}

void vayu_py_decref(int64_t obj) {
    if (!vayu_py_init()) return;
    if (obj && p_Py_DecRef) p_Py_DecRef(obj);
}

void vayu_py_incref(int64_t obj) {
    if (!vayu_py_init()) return;
    if (obj && p_Py_IncRef) p_Py_IncRef(obj);
}

static int64_t vayu_py_arg_to_obj(int64_t v, int8_t tag) {
    if (tag == 0 || tag == 1) {
        return p_PyLong_FromLongLong(v);
    }
    if (tag == 2) {
        VayuStr* s = (VayuStr*)v;
        return p_PyUnicode_FromStringAndSize(s->data, s->len);
    }
    return 0;
}

int64_t vayu_py_call(int64_t fn, int64_t args_list) {
    if (!vayu_py_init() || !p_PyObject_Call || !p_PyTuple_New) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!fn) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call: null function handle"));
    }
    VayuList* lst = (VayuList*)args_list;
    int64_t n = lst->len;
    int64_t tup = p_PyTuple_New(n);
    if (!tup) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call: PyTuple_New failed"));
    }
    for (int64_t i = 0; i < n; ++i) {
        int64_t arg = vayu_py_arg_to_obj(lst->items[i], lst->tags[i]);
        if (!arg) {
            if (p_Py_DecRef) p_Py_DecRef(tup);
            vayu_raise_str(vayu_mkstr_c("TypeError"),
                           vayu_mkstr_c("py.call: unsupported argument type"));
        }
        p_PyTuple_SetItem(tup, i, arg);
    }
    int64_t result = p_PyObject_Call(fn, tup, 0);
    if (p_Py_DecRef) p_Py_DecRef(tup);
    return result;
}

/* ---- Phase 16.3 + 16.4: bidirectional ---- */

/* Additional symbols needed for eval / exec_file / callback / attrs. */
typedef int64_t (*vayu_py_runstring_t)(const char*, int, int64_t, int64_t);
typedef int64_t (*vayu_py_addmodule_t)(const char*);
typedef int64_t (*vayu_py_getdict_t)(int64_t);
typedef int64_t (*vayu_py_cfunc_new_t)(void*, int64_t, int64_t);
typedef int64_t (*vayu_py_capsule_new_t)(void*, const char*, void*);
typedef void*   (*vayu_py_capsule_get_t)(int64_t, const char*);
typedef int64_t (*vayu_py_tuple_size_t)(int64_t);
typedef int64_t (*vayu_py_tuple_getitem_t)(int64_t, int64_t);
typedef void    (*vayu_py_err_clear_t)(void);
typedef int64_t (*vayu_py_setattr_t)(int64_t, const char*, int64_t);
typedef int64_t (*vayu_py_repr_t)(int64_t);
typedef int64_t (*vayu_py_type_t)(int64_t);
typedef int     (*vayu_py_gil_ensure_t)(void);
typedef void    (*vayu_py_gil_release_t)(int);
typedef int64_t (*vayu_py_dict_new_t)(void);
typedef int     (*vayu_py_dict_setstr_t)(int64_t, const char*, int64_t);
typedef int64_t (*vayu_py_dict_size_t)(int64_t);
typedef int64_t (*vayu_py_dict_keys_t)(int64_t);
typedef int64_t (*vayu_py_dict_getitem_t)(int64_t, int64_t);
typedef int64_t (*vayu_py_list_size_t)(int64_t);
typedef int64_t (*vayu_py_list_getitem_t)(int64_t, int64_t);
typedef int64_t (*vayu_py_obj_str_t)(int64_t);
typedef int64_t (*vayu_py_err_occurred_t)(void);
typedef void    (*vayu_py_err_fetch_t)(int64_t*, int64_t*, int64_t*);
typedef void    (*vayu_py_err_norm_t)(int64_t*, int64_t*, int64_t*);

typedef struct {
    int64_t (*fn)(int64_t);
    int      ret_kind;   /* 0=int 1=bool 2=str */
} VayuPyCallback;

static vayu_py_runstring_t     p_PyRun_String          = NULL;
static vayu_py_addmodule_t     p_PyImport_AddModule    = NULL;
static vayu_py_getdict_t       p_PyModule_GetDict      = NULL;
static vayu_py_cfunc_new_t     p_PyCFunction_NewEx     = NULL;
static vayu_py_capsule_new_t   p_PyCapsule_New         = NULL;
static vayu_py_capsule_get_t   p_PyCapsule_GetPointer  = NULL;
static vayu_py_tuple_size_t    p_PyTuple_Size          = NULL;
static vayu_py_tuple_getitem_t p_PyTuple_GetItem       = NULL;
static vayu_py_err_clear_t     p_PyErr_Clear           = NULL;
static vayu_py_setattr_t       p_PyObject_SetAttrString = NULL;
static vayu_py_repr_t          p_PyObject_Repr          = NULL;
static vayu_py_type_t          p_PyObject_Type          = NULL;
static vayu_py_gil_ensure_t    p_PyGILState_Ensure      = NULL;
static vayu_py_gil_release_t   p_PyGILState_Release     = NULL;
static vayu_py_dict_new_t      p_PyDict_New             = NULL;
static vayu_py_dict_setstr_t   p_PyDict_SetItemString   = NULL;
static vayu_py_dict_size_t     p_PyDict_Size            = NULL;
static vayu_py_dict_keys_t     p_PyDict_Keys            = NULL;
static vayu_py_dict_getitem_t  p_PyDict_GetItem         = NULL;
static vayu_py_list_size_t     p_PyList_Size            = NULL;
static vayu_py_list_getitem_t  p_PyList_GetItem         = NULL;
static vayu_py_obj_str_t       p_PyObject_Str           = NULL;
static vayu_py_err_occurred_t  p_PyErr_Occurred         = NULL;
static vayu_py_err_fetch_t     p_PyErr_Fetch            = NULL;
static vayu_py_err_norm_t      p_PyErr_NormalizeException = NULL;

static void* p_PyLong_Type    = NULL;
static void* p_PyBool_Type    = NULL;
static void* p_PyUnicode_Type = NULL;

static int vayu_py_load_bidi(void) {
    if (!vayu_py_load_dll()) return 0;
    if (!p_PyRun_String) {
        p_PyRun_String          = (vayu_py_runstring_t)
            VAYU_PY_SYM(g_py_dll, "PyRun_String");
        p_PyImport_AddModule    = (vayu_py_addmodule_t)
            VAYU_PY_SYM(g_py_dll, "PyImport_AddModule");
        p_PyModule_GetDict      = (vayu_py_getdict_t)
            VAYU_PY_SYM(g_py_dll, "PyModule_GetDict");
        p_PyCFunction_NewEx     = (vayu_py_cfunc_new_t)
            VAYU_PY_SYM(g_py_dll, "PyCFunction_NewEx");
        p_PyCapsule_New         = (vayu_py_capsule_new_t)
            VAYU_PY_SYM(g_py_dll, "PyCapsule_New");
        p_PyCapsule_GetPointer  = (vayu_py_capsule_get_t)
            VAYU_PY_SYM(g_py_dll, "PyCapsule_GetPointer");
        p_PyTuple_Size          = (vayu_py_tuple_size_t)
            VAYU_PY_SYM(g_py_dll, "PyTuple_Size");
        p_PyTuple_GetItem       = (vayu_py_tuple_getitem_t)
            VAYU_PY_SYM(g_py_dll, "PyTuple_GetItem");
        p_PyErr_Clear           = (vayu_py_err_clear_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_Clear");
        p_PyObject_SetAttrString = (vayu_py_setattr_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_SetAttrString");
        p_PyObject_Repr          = (vayu_py_repr_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_Repr");
        p_PyObject_Type          = (vayu_py_type_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_Type");
        p_PyGILState_Ensure      = (vayu_py_gil_ensure_t)
            VAYU_PY_SYM(g_py_dll, "PyGILState_Ensure");
        p_PyGILState_Release     = (vayu_py_gil_release_t)
            VAYU_PY_SYM(g_py_dll, "PyGILState_Release");
        p_PyLong_Type    = VAYU_PY_SYM(g_py_dll, "PyLong_Type");
        p_PyBool_Type    = VAYU_PY_SYM(g_py_dll, "PyBool_Type");
        p_PyUnicode_Type = VAYU_PY_SYM(g_py_dll, "PyUnicode_Type");
                p_PyDict_New              = (vayu_py_dict_new_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_New");
        p_PyDict_SetItemString    = (vayu_py_dict_setstr_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_SetItemString");
        p_PyDict_Size             = (vayu_py_dict_size_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_Size");
        p_PyDict_Keys             = (vayu_py_dict_keys_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_Keys");
        p_PyDict_GetItem          = (vayu_py_dict_getitem_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_GetItem");
        p_PyList_Size             = (vayu_py_list_size_t)
            VAYU_PY_SYM(g_py_dll, "PyList_Size");
        p_PyList_GetItem          = (vayu_py_list_getitem_t)
            VAYU_PY_SYM(g_py_dll, "PyList_GetItem");
        p_PyObject_Str            = (vayu_py_obj_str_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_Str");
        p_PyErr_Occurred          = (vayu_py_err_occurred_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_Occurred");
        p_PyErr_Fetch             = (vayu_py_err_fetch_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_Fetch");
        p_PyErr_NormalizeException = (vayu_py_err_norm_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_NormalizeException");
    }
    return p_PyRun_String && p_PyImport_AddModule && p_PyModule_GetDict &&
           p_PyCFunction_NewEx && p_PyCapsule_New && p_PyCapsule_GetPointer &&
           p_PyTuple_Size && p_PyTuple_GetItem &&
           p_PyLong_Type && p_PyBool_Type && p_PyUnicode_Type;
}

/* Py_eval_input is 258 (CPython stable value). */
int64_t vayu_py_eval(int64_t expr_str) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* s = (VayuStr*)expr_str;
    int64_t main_mod = p_PyImport_AddModule("__main__");
    int64_t d        = p_PyModule_GetDict(main_mod);
    int64_t r        = p_PyRun_String(s->data, 258, d, d);
    /* leave error pending for py.last_error() */
    if (!r && p_PyErr_Clear && 0) p_PyErr_Clear();
    return r;
}

void vayu_py_exec_file(int64_t path_str) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* path = (VayuStr*)path_str;
    char pathbuf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(pathbuf, path->data, (size_t)n);
    pathbuf[n] = 0;

    FILE* f = fopen(pathbuf, "rb");
    if (!f) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_concat_c("py.exec_file: cannot open ", path));
    }
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char* buf = (char*)malloc((size_t)sz + 1);
    if (sz > 0) fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);

    if (!vayu_py_init()) {
        free(buf);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    int rc = p_PyRun_SimpleString(buf);
    free(buf);
    if (rc != 0 && p_PyErr_Print) p_PyErr_Print();
}

/* ---- Vayu callbacks callable from Python ---- */

/* PyObject header on 64-bit: ob_refcnt then ob_type. */
static void* vayu_py_type_of(void* obj) {
    return *(void**)((char*)obj + sizeof(void*));
}

static int64_t vayu_py_obj_to_i64(int64_t obj) {
    if (!obj) return 0;
    void* t = vayu_py_type_of((void*)obj);
    if (t == p_PyBool_Type) {
        return (int64_t)(p_PyObject_IsTrue(obj) ? 1 : 0);
    }
    if (t == p_PyLong_Type) {
        return p_PyLong_AsLongLong(obj);
    }
    if (t == p_PyUnicode_Type) {
        const char* s = p_PyUnicode_AsUTF8(obj);
        if (!s) return 0;
        return (int64_t)vayu_mkstr_c(s);
    }
    return 0;
}

static int64_t vayu_py_i64_to_obj(int64_t v, int ret_kind) {
    if (ret_kind == 2) {  /* str */
        VayuStr* s = (VayuStr*)v;
        return p_PyUnicode_FromStringAndSize(s->data, s->len);
    }
    if (ret_kind == 1) {  /* bool */
        return p_PyBool_FromLong(v ? 1 : 0);
    }
    return p_PyLong_FromLongLong(v);
}

static int64_t vayu_py_cb_call(int64_t self, int64_t args);

static void vayu_py_cb_destructor(int64_t cap) {
    VayuPyCallback* cb = (VayuPyCallback*)p_PyCapsule_GetPointer(
        cap, "vayu_py_callback");
    if (cb) free(cb);
}

int64_t vayu_py_callback(int64_t fn_ptr, int64_t ret_kind_str) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!fn_ptr) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("py.callback: null function pointer"));
    }
    VayuStr* rk = (VayuStr*)ret_kind_str;
    int kind = 0;
    if (rk->len == 4 && memcmp(rk->data, "bool", 4) == 0) kind = 1;
    else if (rk->len == 3 && memcmp(rk->data, "str", 3) == 0) kind = 2;

    VayuPyCallback* cb = (VayuPyCallback*)malloc(sizeof(VayuPyCallback));
    cb->fn = (int64_t (*)(int64_t))fn_ptr;
    cb->ret_kind = kind;

    int64_t cap = p_PyCapsule_New(cb, "vayu_py_callback",
                                  (void*)&vayu_py_cb_destructor);
    if (!cap) {
        free(cb);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.callback: PyCapsule_New failed"));
    }

    /* PyMethodDef must outlive the PyCFunction object — leak one. */
    struct VayuMethDef {
        const char* ml_name;
        void*       ml_meth;
        int         ml_flags;
        const char* ml_doc;
    };
    struct VayuMethDef* md =
        (struct VayuMethDef*)malloc(sizeof(struct VayuMethDef));
    md->ml_name  = "vayu_cb";
    md->ml_meth  = (void*)&vayu_py_cb_call;
    md->ml_flags = 0x0001;   /* METH_VARARGS */
    md->ml_doc   = "Vayu callback";

    int64_t fn_obj = p_PyCFunction_NewEx(md, cap, 0);
    if (!fn_obj) {
        free(md);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.callback: PyCFunction_NewEx failed"));
    }
    if (p_Py_DecRef) p_Py_DecRef(cap);
    return fn_obj;
}

static int64_t vayu_py_cb_call(int64_t self, int64_t args) {
    VayuPyCallback* cb = (VayuPyCallback*)p_PyCapsule_GetPointer(
        self, "vayu_py_callback");
    if (!cb) return 0;
    /* 16.4: GIL guard so Vayu callbacks invoked from worker threads work. */
    int gil = 0;
    if (p_PyGILState_Ensure) gil = p_PyGILState_Ensure();
    int64_t n = p_PyTuple_Size(args);
    int64_t a0 = 0;
    if (n >= 1) {
        int64_t o = p_PyTuple_GetItem(args, 0);
        a0 = vayu_py_obj_to_i64(o);
    }
    int64_t r = cb->fn(a0);
    int64_t out = vayu_py_i64_to_obj(r, cb->ret_kind);
    if (p_PyGILState_Release) p_PyGILState_Release(gil);
    return out;
}

/* ---- Phase 16.4: attributes / repr / type_name ---- */

int64_t vayu_py_setattr(int64_t obj, int64_t name_str, int64_t value_obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* n = (VayuStr*)name_str;
    char namebuf[512];
    int64_t ln = n->len < 511 ? n->len : 511;
    memcpy(namebuf, n->data, (size_t)ln);
    namebuf[ln] = 0;
    int rc = (int)p_PyObject_SetAttrString(obj, namebuf, value_obj);
    if (rc != 0) {
        if (p_PyErr_Print) p_PyErr_Print();
        return 0;
    }
    return 1;
}

int64_t vayu_py_repr(int64_t obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyObject_Repr(obj);
}

int64_t vayu_py_type_name(int64_t obj) {
    if (!vayu_py_load_bidi() || !p_PyObject_GetAttrString) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!obj) return (int64_t)vayu_mkstr_c("NoneType");
    int64_t tp = p_PyObject_Type(obj);
    if (!tp) return (int64_t)vayu_mkstr_c("?");
    int64_t nm = p_PyObject_GetAttrString(tp, "__name__");
    if (!nm) {
        if (p_Py_DecRef) p_Py_DecRef(tp);
        return (int64_t)vayu_mkstr_c("?");
    }
    const char* s = p_PyUnicode_AsUTF8(nm);
    VayuStr* out = vayu_mkstr_c(s ? s : "?");
    if (p_Py_DecRef) { p_Py_DecRef(nm); p_Py_DecRef(tp); }
    return (int64_t)out;
}

/* ---- Phase 16.5: kwargs, list/dict bridging, last_error ---- */

int64_t vayu_py_call_kw(int64_t fn, int64_t kwargs_map) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!fn) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call_kw: null function handle"));
    }
    VayuMap* m = (VayuMap*)kwargs_map;
    int64_t d = p_PyDict_New();
    if (!d) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call_kw: PyDict_New failed"));
    }
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        VayuStr* k = m->entries[i].key;
        char kbuf[512];
        int64_t kn = k->len < 511 ? k->len : 511;
        memcpy(kbuf, k->data, (size_t)kn);
        kbuf[kn] = 0;
        int rc = p_PyDict_SetItemString(d, kbuf, m->entries[i].value);
        if (rc != 0) {
            if (p_Py_DecRef) p_Py_DecRef(d);
            vayu_raise_str(vayu_mkstr_c("TypeError"),
                           vayu_mkstr_c("py.call_kw: bad kwarg name"));
        }
    }
    int64_t tup = p_PyTuple_New(0);
    int64_t r = p_PyObject_Call(fn, tup, d);
    if (p_Py_DecRef) { p_Py_DecRef(tup); p_Py_DecRef(d); }
    /* leave the error pending so py.last_error() can read it */
    return r;
}

VayuList* vayu_py_list(int64_t obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuList* out = vayu_list_new();
    if (!obj || !p_PyList_Size) return out;
    int64_t n = p_PyList_Size(obj);
    if (n < 0) { if (p_PyErr_Clear) p_PyErr_Clear(); return out; }
    for (int64_t i = 0; i < n; ++i) {
        int64_t v = p_PyList_GetItem(obj, i);
        vayu_list_push_tagged(out, v, 0);
    }
    return out;
}

VayuMap* vayu_py_dict(int64_t obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuMap* out = vayu_map_new();
    if (!obj || !p_PyDict_Size) return out;
    int64_t keys = p_PyDict_Keys(obj);
    if (!keys) {
        if (p_PyErr_Clear) p_PyErr_Clear();
        return out;
    }
    int64_t n = p_PyList_Size(keys);
    for (int64_t i = 0; i < n; ++i) {
        int64_t k = p_PyList_GetItem(keys, i);
        int64_t v = p_PyDict_GetItem(obj, k);
        const char* ks = p_PyUnicode_AsUTF8(k);
        if (!ks) continue;
        vayu_map_put(out, vayu_mkstr_c(ks), v);
    }
    if (p_Py_DecRef) p_Py_DecRef(keys);
    return out;
}

VayuStr* vayu_py_last_error(void) {
    if (!vayu_py_load_bidi()) return vayu_mkstr("", 0);
    if (!p_PyErr_Occurred || !p_PyErr_Occurred()) return vayu_mkstr("", 0);
    int64_t tp = 0, val = 0, tb = 0;
    p_PyErr_Fetch(&tp, &val, &tb);
    if (p_PyErr_NormalizeException)
        p_PyErr_NormalizeException(&tp, &val, &tb);
    VayuStr* out = vayu_mkstr_c("(unknown error)");
    if (val && p_PyObject_Str) {
        int64_t s = p_PyObject_Str(val);
        if (s) {
            const char* cs = p_PyUnicode_AsUTF8(s);
            if (cs) out = vayu_mkstr_c(cs);
            if (p_Py_DecRef) p_Py_DecRef(s);
        }
    }
    if (tp && p_Py_DecRef) p_Py_DecRef(tp);
    if (val && p_Py_DecRef) p_Py_DecRef(val);
    if (tb && p_Py_DecRef) p_Py_DecRef(tb);
    return out;
}

/* ---- Phase 15.2d: compound lvalue addresses ---- */
int64_t* vayu_list_slot(VayuList* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    return &l->items[i];
}
int64_t* vayu_map_slot(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k);
    if (e) return &e->value;
    map_grow(m);
    uint64_t h = hash_str(k) & (uint64_t)(m->cap - 1);
    while (m->entries[h].used) h = (h + 1) & (uint64_t)(m->cap - 1);
    m->entries[h].used = 1;
    m->entries[h].key = k;
    m->entries[h].value = 0;
    m->len++;
    return &m->entries[h].value;
}

/* ---- Phase 15.4: C callback test helper ---- */
int64_t vayu_test_apply(int64_t (*f)(int64_t), int64_t x) {
    if (!f) return 0;
    return f(x);
}

/* ---- Phase 15.5: FFI by-value single-field structs ---- */
typedef struct { int64_t v; } VayuWrap8;

VayuWrap8 vayu_ffi_wrap_double(VayuWrap8 w) {
    VayuWrap8 r;
    r.v = w.v * 2;
    return r;
}

int64_t vayu_ffi_wrap_sum(VayuWrap8 a, VayuWrap8 b) {
    return a.v + b.v;
}

/* ---- Phase 15.2b / 15.3: FFI test helpers ---- */
int64_t vayu_ffi_deref(int64_t p) { return *(int64_t*)p; }
void vayu_ffi_store(int64_t p, int64_t v) { *(int64_t*)p = v; }

void* vayu_ffi_point_new(int64_t x, int64_t y) {
    int64_t* p = (int64_t*)malloc(16);
    p[0] = x;
    p[1] = y;
    return p;
}
int64_t vayu_ffi_point_sum(void* p) {
    int64_t* ip = (int64_t*)p;
    return ip[0] + ip[1];
}

VayuStr* vayu_read_line(void) {
    size_t cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) {
        free(buf);
        return vayu_mkstr("", 0);
    }
    if (len > 0 && buf[len - 1] == '\r') --len;
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + len + 1);
    s->len = (int64_t)len;
    memcpy(s->data, buf, len);
    s->data[len] = 0;
    free(buf);
    return s;
}

VayuStr* vayu_read_all(void) {
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
    }
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + len + 1);
    s->len = (int64_t)len;
    memcpy(s->data, buf, len);
    s->data[len] = 0;
    free(buf);
    return s;
}

int64_t vayu_read_int(void) {
    VayuStr* s = vayu_read_line();
    return vayu_str_to_int(s);
}

VayuStr* vayu_input_plain(void) {
    return vayu_read_line();
}

VayuStr* vayu_input_prompt(VayuStr* prompt) {
    if (prompt->len) fwrite(prompt->data, 1, (size_t)prompt->len, stdout);
    fflush(stdout);
    return vayu_read_line();
}

static VayuStr* vayu_concat_c(const char* prefix, VayuStr* s) {
    int64_t plen = (int64_t)strlen(prefix);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)(plen + s->len) + 1);
    r->len = plen + s->len;
    if (plen)   memcpy(r->data, prefix, (size_t)plen);
    if (s->len) memcpy(r->data + plen, s->data, (size_t)s->len);
    r->data[r->len] = 0;
    return r;
}

VayuStr* vayu_str_concat(VayuStr* a, VayuStr* b) {
    int64_t n = a->len + b->len;
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)n + 1);
    s->len = n;
    if (a->len) memcpy(s->data, a->data, (size_t)a->len);
    if (b->len) memcpy(s->data + a->len, b->data, (size_t)b->len);
    s->data[n] = 0;
    return s;
}
int64_t vayu_str_eq(VayuStr* a, VayuStr* b) {
    if (a == b) return 1;
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, (size_t)a->len) == 0;
}
int64_t vayu_str_ne(VayuStr* a, VayuStr* b) { return !vayu_str_eq(a, b); }
int64_t vayu_str_len(VayuStr* s) { return s->len; }

VayuStr* vayu_int_to_str(long long v, long long kind) {
    char buf[64]; int n;
    if (kind == 1) n = snprintf(buf, sizeof(buf), "%s", v ? "true" : "false");
    else           n = snprintf(buf, sizeof(buf), "%lld", v);
    return vayu_mkstr(buf, n);
}
VayuStr* vayu_str_upper(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        r->data[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    r->data[s->len] = 0;
    return r;
}
VayuStr* vayu_str_lower(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    r->data[s->len] = 0;
    return r;
}
int64_t vayu_str_contains(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return 1;
    if (sub->len > s->len) return 0;
    for (int64_t i = 0; i + sub->len <= s->len; ++i)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return 1;
    return 0;
}
int64_t vayu_str_find(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return 0;
    if (sub->len > s->len) return -1;
    for (int64_t i = 0; i + sub->len <= s->len; ++i)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
    return -1;
}
int64_t vayu_str_starts_with(VayuStr* s, VayuStr* p) {
    if (p->len > s->len) return 0;
    return memcmp(s->data, p->data, (size_t)p->len) == 0;
}
int64_t vayu_str_ends_with(VayuStr* s, VayuStr* p) {
    if (p->len > s->len) return 0;
    return memcmp(s->data + (s->len - p->len), p->data, (size_t)p->len) == 0;
}

VayuStr* vayu_str_char_at(VayuStr* s, int64_t i) {
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("string index out of range"));
    }
    return vayu_mkstr(s->data + i, 1);
}
VayuStr* vayu_str_substr(VayuStr* s, int64_t start, int64_t end) {
    if (start < 0) start += s->len;
    if (end   < 0) end   += s->len;
    if (start < 0) start = 0;
    if (end   > s->len) end = s->len;
    if (end < start) end = start;
    return vayu_mkstr(s->data + start, end - start);
}
VayuStr* vayu_str_capitalize(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (s->len > 0) {
        char c = r->data[0];
        if (c >= 'a' && c <= 'z') r->data[0] = (char)(c - 32);
    }
    r->data[s->len] = 0;
    return r;
}

VayuStr* vayu_str_title(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    int atStart = 1;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        int isWS = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                    c == '\f' || c == '\v');
        if (isWS) { r->data[i] = (char)c; atStart = 1; }
        else if (atStart) {
            r->data[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
            atStart = 0;
        } else {
            r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
        }
    }
    r->data[s->len] = 0;
    return r;
}

VayuStr* vayu_str_swapcase(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (c >= 'a' && c <= 'z')      r->data[i] = (char)(c - 32);
        else if (c >= 'A' && c <= 'Z') r->data[i] = (char)(c + 32);
        else                           r->data[i] = c;
    }
    r->data[s->len] = 0;
    return r;
}

static VayuStr* vayu_str_pad_common(VayuStr* s, int64_t width,
                                    VayuStr* fill, int mode) {
    if (fill->len == 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("fill character must not be empty"));
    }
    int64_t pad = width - s->len;
    if (pad <= 0) return vayu_mkstr(s->data, s->len);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) +
                                  (size_t)(s->len + pad) + 1);
    r->len = s->len + pad;
    int64_t lp = 0, rp = 0;
    if (mode == 1)      rp = pad;
    else if (mode == 2) lp = pad;
    else { lp = pad / 2; rp = pad - lp; }
    int64_t op = 0;
    for (int64_t i = 0; i < lp; ++i)
        r->data[op++] = fill->data[i % fill->len];
    if (s->len) memcpy(r->data + op, s->data, (size_t)s->len);
    op += s->len;
    for (int64_t i = 0; i < rp; ++i)
        r->data[op++] = fill->data[i % fill->len];
    r->data[op] = 0;
    return r;
}
VayuStr* vayu_str_center(VayuStr* s, int64_t w, VayuStr* f) {
    return vayu_str_pad_common(s, w, f, 0);
}
VayuStr* vayu_str_ljust(VayuStr* s, int64_t w, VayuStr* f) {
    return vayu_str_pad_common(s, w, f, 1);
}
VayuStr* vayu_str_rjust(VayuStr* s, int64_t w, VayuStr* f) {
    return vayu_str_pad_common(s, w, f, 2);
}

VayuStr* vayu_str_zfill(VayuStr* s, int64_t w) {
    int64_t pad = w - s->len;
    if (pad <= 0) return vayu_mkstr(s->data, s->len);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)w + 1);
    r->len = w;
    int64_t op = 0, start = 0;
    if (s->len > 0 && (s->data[0] == '-' || s->data[0] == '+')) {
        r->data[op++] = s->data[0];
        start = 1;
    }
    for (int64_t i = 0; i < pad; ++i) r->data[op++] = '0';
    if (s->len > start) {
        memcpy(r->data + op, s->data + start, (size_t)(s->len - start));
        op += s->len - start;
    }
    r->data[op] = 0;
    return r;
}

int64_t vayu_str_count(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return s->len + 1;
    int64_t count = 0;
    for (int64_t i = 0; i + sub->len <= s->len; ) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) {
            ++count; i += sub->len;
        } else ++i;
    }
    return count;
}

int64_t vayu_str_rfind(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return s->len;
    if (sub->len > s->len) return -1;
    for (int64_t i = s->len - sub->len; i >= 0; --i) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
    }
    return -1;
}

VayuList* vayu_str_partition(VayuStr* s, VayuStr* sep, int64_t right) {
    VayuList* out = vayu_list_new();
    if (sep->len == 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("empty separator"));
    }
    int64_t p = -1;
    if (right) {
        for (int64_t i = s->len - sep->len; i >= 0; --i) {
            if (memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) {
                p = i; break;
            }
        }
    } else {
        for (int64_t i = 0; i + sep->len <= s->len; ++i) {
            if (memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) {
                p = i; break;
            }
        }
    }
    if (p < 0) {
        if (right) {
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data, s->len), 2);
        } else {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data, s->len), 2);
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
        }
        return out;
    }
    vayu_list_push_tagged(out, (int64_t)vayu_mkstr(s->data, p), 2);
    vayu_list_push_tagged(out, (int64_t)vayu_mkstr(sep->data, sep->len), 2);
    vayu_list_push_tagged(out,
        (int64_t)vayu_mkstr(s->data + p + sep->len,
                            s->len - p - sep->len), 2);
    return out;
}

VayuList* vayu_str_splitlines(VayuStr* s) {
    VayuList* out = vayu_list_new();
    int64_t start = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (c == '\n') {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data + start, i - start), 2);
            start = i + 1;
        } else if (c == '\r') {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data + start, i - start), 2);
            if (i + 1 < s->len && s->data[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    if (start < s->len)
        vayu_list_push_tagged(out,
            (int64_t)vayu_mkstr(s->data + start, s->len - start), 2);
    return out;
}

VayuList* vayu_str_rsplit(VayuStr* s, VayuStr* sep, int64_t maxsplit) {
    VayuList* out = vayu_list_new();
    VayuStr** parts = NULL;
    int64_t n = 0, cap = 0;
    if (sep->len == 0) {
        int64_t end = s->len;
        while (end > 0) {
            if (maxsplit >= 0 && n >= maxsplit) break;
            while (end > 0 && (s->data[end-1]==' ' || s->data[end-1]=='\t' ||
                   s->data[end-1]=='\n' || s->data[end-1]=='\r' ||
                   s->data[end-1]=='\f' || s->data[end-1]=='\v')) --end;
            if (end == 0) break;
            int64_t st = end;
            while (st > 0 && !(s->data[st-1]==' ' || s->data[st-1]=='\t' ||
                   s->data[st-1]=='\n' || s->data[st-1]=='\r' ||
                   s->data[st-1]=='\f' || s->data[st-1]=='\v')) --st;
            if (n == cap) { cap = cap ? cap*2 : 4;
                parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
            parts[n++] = vayu_mkstr(s->data + st, end - st);
            end = st;
        }
        if (end > 0) {
            int64_t a = 0;
            while (a < end && (s->data[a]==' ' || s->data[a]=='\t' ||
                   s->data[a]=='\n' || s->data[a]=='\r' ||
                   s->data[a]=='\f' || s->data[a]=='\v')) ++a;
            if (n == cap) { cap = cap ? cap*2 : 4;
                parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
            parts[n++] = vayu_mkstr(s->data + a, end - a);
        }
    } else {
        int64_t end = s->len;
        while (maxsplit < 0 || n < maxsplit) {
            int64_t p = -1;
            if (end >= sep->len) {
                for (int64_t i = end - sep->len; i >= 0; --i) {
                    if (memcmp(s->data + i, sep->data,
                               (size_t)sep->len) == 0) { p = i; break; }
                }
            }
            if (p < 0) break;
            if (n == cap) { cap = cap ? cap*2 : 4;
                parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
            parts[n++] = vayu_mkstr(s->data + p + sep->len,
                                    end - p - sep->len);
            end = p;
        }
        if (n == cap) { cap = cap ? cap*2 : 4;
            parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
        parts[n++] = vayu_mkstr(s->data, end);
    }
    for (int64_t i = n - 1; i >= 0; --i)
        vayu_list_push_tagged(out, (int64_t)parts[i], 2);
    free(parts);
    return out;
}

int64_t vayu_str_is_lower(VayuStr* s) {
    int any = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (c >= 'A' && c <= 'Z') return 0;
        if (c >= 'a' && c <= 'z') any = 1;
    }
    return any;
}
int64_t vayu_str_is_upper(VayuStr* s) {
    int any = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (c >= 'a' && c <= 'z') return 0;
        if (c >= 'A' && c <= 'Z') any = 1;
    }
    return any;
}
int64_t vayu_str_is_alnum(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z'))) return 0;
    }
    return 1;
}
int64_t vayu_str_is_ascii(VayuStr* s) {
    for (int64_t i = 0; i < s->len; ++i)
        if ((unsigned char)s->data[i] > 127) return 0;
    return 1;
}
VayuStr* vayu_str_replace(VayuStr* s, VayuStr* from, VayuStr* to) {
    if (from->len == 0) return vayu_mkstr(s->data, s->len);
    int64_t count = 0;
    for (int64_t i = 0; i + from->len <= s->len; ) {
        if (memcmp(s->data + i, from->data, (size_t)from->len) == 0) {
            ++count; i += from->len;
        } else ++i;
    }
    int64_t newLen = s->len + count * (to->len - from->len);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)newLen + 1);
    r->len = newLen;
    int64_t op = 0, ip = 0;
    while (ip < s->len) {
        if (ip + from->len <= s->len &&
            memcmp(s->data + ip, from->data, (size_t)from->len) == 0) {
            if (to->len) memcpy(r->data + op, to->data, (size_t)to->len);
            op += to->len;
            ip += from->len;
        } else {
            r->data[op++] = s->data[ip++];
        }
    }
    r->data[newLen] = 0;
    return r;
}
VayuStr* vayu_str_strip(VayuStr* s) {
    int64_t a = 0, b = s->len;
    while (a < b && (s->data[a]==' '||s->data[a]=='\t'||s->data[a]=='\n'||
                     s->data[a]=='\r'||s->data[a]=='\f'||s->data[a]=='\v')) ++a;
    while (b > a && (s->data[b-1]==' '||s->data[b-1]=='\t'||s->data[b-1]=='\n'||
                     s->data[b-1]=='\r'||s->data[b-1]=='\f'||s->data[b-1]=='\v')) --b;
    return vayu_mkstr(s->data + a, b - a);
}
VayuStr* vayu_str_lstrip(VayuStr* s) {
    int64_t a = 0;
    while (a < s->len && (s->data[a]==' '||s->data[a]=='\t'||s->data[a]=='\n'||
                          s->data[a]=='\r'||s->data[a]=='\f'||s->data[a]=='\v')) ++a;
    return vayu_mkstr(s->data + a, s->len - a);
}
VayuStr* vayu_str_rstrip(VayuStr* s) {
    int64_t b = s->len;
    while (b > 0 && (s->data[b-1]==' '||s->data[b-1]=='\t'||s->data[b-1]=='\n'||
                     s->data[b-1]=='\r'||s->data[b-1]=='\f'||s->data[b-1]=='\v')) --b;
    return vayu_mkstr(s->data, b);
}
int64_t vayu_str_is_digit(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!isdigit(c)) return 0;
    }
    return 1;
}
int64_t vayu_str_is_alpha(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!isalpha(c)) return 0;
    }
    return 1;
}
int64_t vayu_str_is_space(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!isspace(c)) return 0;
    }
    return 1;
}
int64_t vayu_str_to_int(VayuStr* s) {
    int64_t n = 0;
    int64_t i = 0;
    int     neg = 0;
    while (i < s->len && (s->data[i]==' '||s->data[i]=='\t')) ++i;
    if (i < s->len && (s->data[i]=='-'||s->data[i]=='+')) {
        neg = (s->data[i]=='-'); ++i;
    }
    for (; i < s->len; ++i) {
        char c = s->data[i];
        if (c < '0' || c > '9') break;
        n = n * 10 + (c - '0');
    }
    return neg ? -n : n;
}

int64_t vayu_ord(VayuStr* s) {
    if (s->len != 1) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("ord() requires a length-1 string"));
    }
    return (int64_t)(unsigned char)s->data[0];
}
VayuStr* vayu_chr(int64_t n) {
    if (n < 0 || n > 255) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("chr() argument out of range"));
    }
    char c = (char)n;
    return vayu_mkstr(&c, 1);
}

VayuList* vayu_list_new() {
    VayuList* l = (VayuList*)malloc(sizeof(VayuList));
    l->len = 0; l->cap = 4;
    l->items = (int64_t*)malloc(sizeof(int64_t) * 4);
    l->tags  = (int8_t*)malloc(4);
    return l;
}
static void vayu_list_grow(VayuList* l) {
    if (l->len < l->cap) return;
    l->cap *= 2;
    l->items = (int64_t*)realloc(l->items, sizeof(int64_t) * (size_t)l->cap);
    l->tags  = (int8_t*)realloc(l->tags, (size_t)l->cap);
}
void vayu_list_push(VayuList* l, int64_t v) {
    vayu_list_grow(l);
    l->items[l->len] = v;
    l->tags[l->len]  = 0;
    l->len++;
}
/* Phase 13.3 fix: retag every element of a list.  Used at the boundary
   of a generic function call, where the erasure of `T` inside the body
   left the wrong tags on the result.  Caller-side code knows the actual
   element type and can restore the correct tag. */
void vayu_list_retag(VayuList* l, int64_t tag) {
    for (int64_t i = 0; i < l->len; ++i) l->tags[i] = (int8_t)tag;
}
void vayu_list_push_tagged(VayuList* l, int64_t v, int64_t tag) {
    vayu_list_grow(l);
    l->items[l->len] = v;
    l->tags[l->len]  = (int8_t)tag;
    l->len++;
}
int64_t vayu_list_get(VayuList* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    return l->items[i];
}
void vayu_list_set(VayuList* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    l->items[i] = v;
    l->tags[i]  = 0;
}
void vayu_list_set_tagged(VayuList* l, int64_t i, int64_t v, int64_t tag) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    l->items[i] = v;
    l->tags[i]  = (int8_t)tag;
}
int64_t vayu_list_pop(VayuList* l) {
    if (l->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("pop from empty list"));
    }
    return l->items[--l->len];
}
int64_t vayu_list_len(VayuList* l) { return l->len; }
void vayu_list_clear(VayuList* l) { l->len = 0; }
int64_t vayu_list_contains(VayuList* l, int64_t v) {
    for (int64_t i = 0; i < l->len; ++i) if (l->items[i] == v) return 1;
    return 0;
}
void vayu_list_insert_tagged(VayuList* l, int64_t i, int64_t v, int64_t tag) {
    if (i < 0) i = 0;
    if (i > l->len) i = l->len;
    vayu_list_grow(l);
    memmove(l->items + i + 1, l->items + i,
            sizeof(int64_t) * (size_t)(l->len - i));
    memmove(l->tags + i + 1, l->tags + i,
            sizeof(int8_t) * (size_t)(l->len - i));
    l->items[i] = v;
    l->tags[i]  = (int8_t)tag;
    l->len++;
}
void vayu_list_insert(VayuList* l, int64_t i, int64_t v) {
    vayu_list_insert_tagged(l, i, v, 0);
}
void vayu_list_remove(VayuList* l, int64_t v) {
    for (int64_t i = 0; i < l->len; ++i) {
        if (l->items[i] == v) {
            memmove(l->items + i, l->items + i + 1,
                    sizeof(int64_t) * (size_t)(l->len - i - 1));
            memmove(l->tags + i, l->tags + i + 1,
                    sizeof(int8_t) * (size_t)(l->len - i - 1));
            l->len--;
            return;
        }
    }
}

void vayu_list_extend(VayuList* dst, VayuList* src) {
    for (int64_t i = 0; i < src->len; ++i) {
        vayu_list_grow(dst);
        dst->items[dst->len] = src->items[i];
        dst->tags[dst->len]  = src->tags[i];
        dst->len++;
    }
}

static int vayu_list_val_eq(int64_t a, int8_t at, int64_t b, int8_t bt) {
    if ((at == 0 || at == 1) && (bt == 0 || bt == 1)) return a == b;
    if (at != bt) return 0;
    if (at == 2) return vayu_str_eq((VayuStr*)a, (VayuStr*)b);
    return a == b;
}

int64_t vayu_list_count(VayuList* l, int64_t v, int64_t tag) {
    int64_t c = 0;
    for (int64_t i = 0; i < l->len; ++i)
        if (vayu_list_val_eq(l->items[i], l->tags[i], v, (int8_t)tag)) ++c;
    return c;
}

void vayu_list_reverse(VayuList* l) {
    for (int64_t i = 0, j = l->len - 1; i < j; ++i, --j) {
        int64_t t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t;
        int8_t  s = l->tags[i];  l->tags[i]  = l->tags[j];  l->tags[j]  = s;
    }
}

static int vayu_list_cmp_slot(VayuList* l, int64_t ia, int64_t ib, int allNum) {
    if (allNum) {
        int64_t a = l->items[ia], b = l->items[ib];
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    VayuStr* sa = (VayuStr*)l->items[ia];
    VayuStr* sb = (VayuStr*)l->items[ib];
    int64_t n = sa->len < sb->len ? sa->len : sb->len;
    int mc = memcmp(sa->data, sb->data, (size_t)n);
    if (mc < 0) return -1;
    if (mc > 0) return 1;
    return (sa->len < sb->len) ? -1 : (sa->len > sb->len) ? 1 : 0;
}

static void vayu_list_merge(VayuList* l, int64_t* tmpI, int8_t* tmpT,
                            int64_t lo, int64_t mid, int64_t hi, int allNum) {
    int64_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (vayu_list_cmp_slot(l, i, j, allNum) <= 0) {
            tmpI[k] = l->items[i]; tmpT[k] = l->tags[i]; ++i;
        } else {
            tmpI[k] = l->items[j]; tmpT[k] = l->tags[j]; ++j;
        }
        ++k;
    }
    while (i < mid) { tmpI[k] = l->items[i]; tmpT[k] = l->tags[i]; ++i; ++k; }
    while (j < hi)  { tmpI[k] = l->items[j]; tmpT[k] = l->tags[j]; ++j; ++k; }
    for (int64_t p = lo; p < hi; ++p) {
        l->items[p] = tmpI[p];
        l->tags[p]  = tmpT[p];
    }
}

void vayu_list_sort(VayuList* l) {
    if (l->len <= 1) return;
    int allNum = 1, allStr = 1;
    for (int64_t i = 0; i < l->len; ++i) {
        int8_t t = l->tags[i];
        if (t == 3 || t == 4) { allNum = 0; allStr = 0; }
        else if (t == 2)      { allNum = 0; }
        else                  { allStr = 0; }
        if (!allNum && !allStr) break;
    }
    if (!allNum && !allStr) {
        vayu_raise_str(vayu_mkstr_c("TypeError"),
                       vayu_mkstr_c("list.sort(): elements are not comparable"));
    }
    int64_t* tmpI = (int64_t*)malloc(sizeof(int64_t) * (size_t)l->len);
    int8_t*  tmpT = (int8_t*)malloc((size_t)l->len);
    int64_t width = 1;
    while (width < l->len) {
        for (int64_t i = 0; i < l->len; i += width * 2) {
            int64_t mid = i + width;
            int64_t hi  = i + width * 2;
            if (mid > l->len) mid = l->len;
            if (hi  > l->len) hi  = l->len;
            if (mid < hi)
                vayu_list_merge(l, tmpI, tmpT, i, mid, hi, allNum);
        }
        width *= 2;
    }
    free(tmpI);
    free(tmpT);
}

VayuList* vayu_list_copy(VayuList* l) {
    VayuList* r = (VayuList*)malloc(sizeof(VayuList));
    r->len = l->len;
    r->cap = l->len < 4 ? 4 : l->len;
    r->items = (int64_t*)malloc(sizeof(int64_t) * (size_t)r->cap);
    r->tags  = (int8_t*)malloc((size_t)r->cap);
    if (l->len) {
        memcpy(r->items, l->items, sizeof(int64_t) * (size_t)l->len);
        memcpy(r->tags,  l->tags,  (size_t)l->len);
    }
    return r;
}

/* ---- Phase 14.0: tuples (VayuList with tag 5) ---- */
typedef VayuList VayuTuple;

VayuTuple* vayu_tuple_new(void) { return vayu_list_new(); }
void vayu_tuple_push_tagged(VayuTuple* t, int64_t v, int64_t tag) {
    vayu_list_push_tagged(t, v, tag);
}
int64_t vayu_tuple_get(VayuTuple* t, int64_t i) {
    return vayu_list_get(t, i);
}
int64_t vayu_tuple_len(VayuTuple* t) { return t->len; }
VayuTuple* vayu_tuple_from_list(VayuList* l) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < l->len; ++i)
        vayu_list_push_tagged(r, l->items[i], l->tags[i]);
    return r;
}
int64_t vayu_tuple_count(VayuTuple* t, int64_t v, int64_t tag) {
    int64_t c = 0;
    for (int64_t i = 0; i < t->len; ++i)
        if (vayu_list_val_eq(t->items[i], t->tags[i], v, (int8_t)tag)) ++c;
    return c;
}

int64_t vayu_tuple_index(VayuTuple* t, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < t->len; ++i)
        if (vayu_list_val_eq(t->items[i], t->tags[i], v, (int8_t)tag)) return i;
    vayu_raise_str(vayu_mkstr_c("ValueError"),
                   vayu_mkstr_c("tuple.index(): value not in tuple"));
    return -1;
}

VayuTuple* vayu_tuple_from_str(VayuStr* s) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < s->len; ++i)
        vayu_list_push_tagged(r, (int64_t)vayu_mkstr(s->data + i, 1), 2);
    return r;
}

static int64_t vayu_list_struct_eq(VayuList* a, VayuList* b) {
    if (a->len != b->len) return 0;
    for (int64_t i = 0; i < a->len; ++i) {
        int8_t ta = a->tags[i], tb = b->tags[i];
        if (ta != tb) return 0;
        if (ta == 2) {
            if (!vayu_str_eq((VayuStr*)a->items[i], (VayuStr*)b->items[i]))
                return 0;
        } else if (ta == 3 || ta == 5 || ta == 6) {
            if (!vayu_list_struct_eq((VayuList*)a->items[i],
                                     (VayuList*)b->items[i])) return 0;
        } else {
            if (a->items[i] != b->items[i]) return 0;
        }
    }
    return 1;
}
int64_t vayu_tuple_eq(VayuTuple* a, VayuTuple* b) {
    return vayu_list_struct_eq(a, b);
}
int64_t vayu_tuple_ne(VayuTuple* a, VayuTuple* b) {
    return !vayu_list_struct_eq(a, b);
}
/* ---- Phase 14.1: sets (VayuList with tag 6, dedup on push) ---- */
typedef VayuList VayuSet;

VayuSet* vayu_set_new(void) { return vayu_list_new(); }
void vayu_set_push_tagged(VayuSet* s, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < s->len; ++i)
        if (vayu_list_val_eq(s->items[i], s->tags[i], v, (int8_t)tag)) return;
    vayu_list_push_tagged(s, v, tag);
}
int64_t vayu_set_has(VayuSet* s, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < s->len; ++i)
        if (vayu_list_val_eq(s->items[i], s->tags[i], v, (int8_t)tag)) return 1;
    return 0;
}
void vayu_set_remove(VayuSet* s, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < s->len; ++i) {
        if (vayu_list_val_eq(s->items[i], s->tags[i], v, (int8_t)tag)) {
            memmove(s->items + i, s->items + i + 1,
                    sizeof(int64_t) * (size_t)(s->len - i - 1));
            memmove(s->tags + i, s->tags + i + 1,
                    (size_t)(s->len - i - 1));
            s->len--;
            return;
        }
    }
}
void vayu_set_clear(VayuSet* s) { s->len = 0; }
VayuSet* vayu_set_union(VayuSet* a, VayuSet* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    for (int64_t i = 0; i < b->len; ++i)
        vayu_set_push_tagged(r, b->items[i], b->tags[i]);
    return r;
}
VayuSet* vayu_set_intersection(VayuSet* a, VayuSet* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        if (vayu_set_has(b, a->items[i], a->tags[i]))
            vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    return r;
}
VayuSet* vayu_set_difference(VayuSet* a, VayuSet* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        if (!vayu_set_has(b, a->items[i], a->tags[i]))
            vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    return r;
}
int64_t vayu_set_len(VayuSet* s) { return s->len; }
VayuSet* vayu_set_copy(VayuSet* s) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < s->len; ++i)
        vayu_list_push_tagged(r, s->items[i], s->tags[i]);
    return r;
}
VayuSet* vayu_set_from_list(VayuList* l) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < l->len; ++i)
        vayu_set_push_tagged(r, l->items[i], l->tags[i]);
    return r;
}
/* ---- Phase 14.4: slicing ---- */
int64_t vayu_slice(int64_t target, int64_t start, int64_t end,
                   int64_t hasStart, int64_t hasEnd, int64_t tag) {
    if (tag == 1) {  /* list */
        VayuList* src = (VayuList*)target;
        int64_t len = src->len;
        if (!hasStart) start = 0;
        else { if (start < 0) start += len; if (start < 0) start = 0;
               if (start > len) start = len; }
        if (!hasEnd) end = len;
        else { if (end < 0) end += len; if (end < 0) end = 0;
               if (end > len) end = len; }
        if (end < start) end = start;
        VayuList* out = vayu_list_new();
        for (int64_t i = start; i < end; ++i)
            vayu_list_push_tagged(out, src->items[i], src->tags[i]);
        return (int64_t)out;
    }
    if (tag == 5) {  /* tuple */
        VayuList* src = (VayuList*)target;
        int64_t len = src->len;
        if (!hasStart) start = 0;
        else { if (start < 0) start += len; if (start < 0) start = 0;
               if (start > len) start = len; }
        if (!hasEnd) end = len;
        else { if (end < 0) end += len; if (end < 0) end = 0;
               if (end > len) end = len; }
        if (end < start) end = start;
        VayuList* out = vayu_list_new();
        for (int64_t i = start; i < end; ++i)
            vayu_list_push_tagged(out, src->items[i], src->tags[i]);
        return (int64_t)out;
    }
    if (tag == 2) {  /* str */
        VayuStr* s = (VayuStr*)target;
        int64_t len = s->len;
        if (!hasStart) start = 0;
        else { if (start < 0) start += len; if (start < 0) start = 0;
               if (start > len) start = len; }
        if (!hasEnd) end = len;
        else { if (end < 0) end += len; if (end < 0) end = 0;
               if (end > len) end = len; }
        if (end < start) end = start;
        return (int64_t)vayu_mkstr(s->data + start, end - start);
    }
    vayu_raise_str(vayu_mkstr_c("TypeError"),
                   vayu_mkstr_c("cannot slice value"));
    return 0;
}

VayuTuple* vayu_tuple_concat(VayuTuple* a, VayuTuple* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    for (int64_t i = 0; i < b->len; ++i)
        vayu_list_push_tagged(r, b->items[i], b->tags[i]);
    return r;
}
int64_t vayu_list_first(VayuList* l) {
    if (l->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("first() on empty list"));
    }
    return l->items[0];
}
int64_t vayu_list_last(VayuList* l) {
    if (l->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("last() on empty list"));
    }
    return l->items[l->len - 1];
}

VayuList* vayu_str_split_ws(VayuStr* s) {
    VayuList* out = vayu_list_new();
    int64_t i = 0;
    while (i < s->len) {
        while (i < s->len) {
            char c = s->data[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
                c != '\f' && c != '\v') break;
            ++i;
        }
        if (i >= s->len) break;
        int64_t start = i;
        while (i < s->len) {
            char c = s->data[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                c == '\f' || c == '\v') break;
            ++i;
        }
        vayu_list_push_tagged(out,
            (int64_t)vayu_mkstr(s->data + start, i - start), 2);
    }
    return out;
}

VayuList* vayu_str_split(VayuStr* s, VayuStr* sep) {
    VayuList* out = vayu_list_new();
    if (sep->len == 0) {
        for (int64_t i = 0; i < s->len; ++i)
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr(s->data + i, 1), 2);
        return out;
    }
    int64_t pos = 0;
    while (pos <= s->len) {
        int64_t next = -1;
        for (int64_t i = pos; i + sep->len <= s->len; ++i) {
            if (memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) {
                next = i; break;
            }
        }
        if (next < 0) {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data + pos, s->len - pos), 2);
            break;
        }
        vayu_list_push_tagged(out,
            (int64_t)vayu_mkstr(s->data + pos, next - pos), 2);
        pos = next + sep->len;
    }
    return out;
}
VayuStr* vayu_str_join(VayuStr* sep, VayuList* parts) {
    int64_t total = 0;
    for (int64_t i = 0; i < parts->len; ++i) {
        VayuStr* p = (VayuStr*)parts->items[i];
        total += p->len;
        if (i + 1 < parts->len) total += sep->len;
    }
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)total + 1);
    r->len = total;
    int64_t op = 0;
    for (int64_t i = 0; i < parts->len; ++i) {
        VayuStr* p = (VayuStr*)parts->items[i];
        if (p->len) memcpy(r->data + op, p->data, (size_t)p->len);
        op += p->len;
        if (i + 1 < parts->len) {
            if (sep->len) memcpy(r->data + op, sep->data, (size_t)sep->len);
            op += sep->len;
        }
    }
    r->data[total] = 0;
    return r;
}

static uint64_t hash_str(VayuStr* s) {
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < s->len; ++i) { h ^= (uint8_t)s->data[i]; h *= 1099511628211ULL; }
    return h;
}
VayuMap* vayu_map_new() {
    VayuMap* m = (VayuMap*)malloc(sizeof(VayuMap));
    m->len = 0; m->cap = 8;
    m->entries = (VayuMapEntry*)calloc((size_t)m->cap, sizeof(VayuMapEntry));
    return m;
}
static VayuMapEntry* map_find(VayuMap* m, VayuStr* k) {
    uint64_t h = hash_str(k) & (uint64_t)(m->cap - 1);
    for (int64_t p = 0; p < m->cap; ++p) {
        VayuMapEntry* e = &m->entries[h];
        if (!e->used) return NULL;
        if (vayu_str_eq(e->key, k)) return e;
        h = (h + 1) & (uint64_t)(m->cap - 1);
    }
    return NULL;
}
static void map_grow(VayuMap* m) {
    if (m->len * 2 < m->cap) return;
    int64_t newCap = m->cap * 2;
    VayuMapEntry* ne = (VayuMapEntry*)calloc((size_t)newCap, sizeof(VayuMapEntry));
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        uint64_t h = hash_str(m->entries[i].key) & (uint64_t)(newCap - 1);
        while (ne[h].used) h = (h + 1) & (uint64_t)(newCap - 1);
        ne[h] = m->entries[i];
    }
    free(m->entries);
    m->entries = ne;
    m->cap = newCap;
}
void vayu_map_put(VayuMap* m, VayuStr* k, int64_t v) {
    VayuMapEntry* e = map_find(m, k);
    if (e) { e->value = v; return; }
    map_grow(m);
    uint64_t h = hash_str(k) & (uint64_t)(m->cap - 1);
    while (m->entries[h].used) h = (h + 1) & (uint64_t)(m->cap - 1);
    m->entries[h].used = 1;
    m->entries[h].key = k;
    m->entries[h].value = v;
    m->len++;
}
int64_t vayu_map_get(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k);
    if (!e) vayu_raise_str(vayu_mkstr_c("KeyError"), k);
    return e->value;
}
int64_t vayu_map_has(VayuMap* m, VayuStr* k) { return map_find(m, k) != NULL; }
void vayu_map_remove(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k); if (e) { e->used = 0; m->len--; }
}
int64_t vayu_map_len(VayuMap* m) { return m->len; }
void vayu_map_clear(VayuMap* m) {
    memset(m->entries, 0, sizeof(VayuMapEntry) * (size_t)m->cap);
    m->len = 0;
}
VayuMap* vayu_map_copy(VayuMap* m) {
    VayuMap* r = (VayuMap*)malloc(sizeof(VayuMap));
    r->len = m->len;
    r->cap = m->cap;
    r->entries = (VayuMapEntry*)calloc((size_t)r->cap, sizeof(VayuMapEntry));
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        r->entries[i] = m->entries[i];
    }
    return r;
}

int64_t vayu_map_get_or(VayuMap* m, VayuStr* k, int64_t def) {
    VayuMapEntry* e = map_find(m, k);
    return e ? e->value : def;
}

int64_t vayu_map_pop(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k);
    if (!e) vayu_raise_str(vayu_mkstr_c("KeyError"), k);
    int64_t v = e->value;
    e->used = 0;
    m->len--;
    return v;
}

int64_t vayu_map_pop_or(VayuMap* m, VayuStr* k, int64_t def) {
    VayuMapEntry* e = map_find(m, k);
    if (!e) return def;
    int64_t v = e->value;
    e->used = 0;
    m->len--;
    return v;
}

void vayu_map_update(VayuMap* dst, VayuMap* src) {
    for (int64_t i = 0; i < src->cap; ++i) {
        if (!src->entries[i].used) continue;
        vayu_map_put(dst, src->entries[i].key, src->entries[i].value);
    }
}

VayuList* vayu_map_values(VayuMap* m, int64_t valTag) {
    VayuList* l = vayu_list_new();
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        vayu_list_push_tagged(l, m->entries[i].value, valTag);
    }
    return l;
}

VayuList* vayu_map_items(VayuMap* m, int64_t valTag) {
    VayuList* l = vayu_list_new();
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        VayuList* pair = vayu_list_new();
        vayu_list_push_tagged(pair, (int64_t)m->entries[i].key, 2);
        vayu_list_push_tagged(pair, m->entries[i].value, valTag);
        vayu_list_push_tagged(l, (int64_t)pair, 3);
    }
    return l;
}
VayuList* vayu_map_keys(VayuMap* m) {
    VayuList* l = vayu_list_new();
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        vayu_list_push_tagged(l, (int64_t)m->entries[i].key, 2);
    }
    return l;
}

int64_t vayu_len(int64_t v, int64_t kind) {
    switch (kind) {
        case 0: return vayu_str_len((VayuStr*)v);
        case 1: return vayu_list_len((VayuList*)v);
        case 2: return vayu_map_len((VayuMap*)v);
        case 3: return vayu_tuple_len((VayuTuple*)v);
        case 4: return vayu_set_len((VayuSet*)v);
    }
    return 0;
}
void vayu_print_value(int64_t v, int64_t kind) {
    switch (kind) {
        case 0: printf("%lld", (long long)v); break;
        case 1: printf("%s", v ? "true" : "false"); break;
        case 2: { VayuStr* s = (VayuStr*)v; fwrite(s->data, 1, (size_t)s->len, stdout); break; }
        case 7: vayu_print_float_noln(v); break;
    }
}

void vayu_print_list_noln(VayuList* l);
void vayu_print_map_noln(VayuMap* m, int64_t vk);
void vayu_print_tuple_noln(VayuList* t);
void vayu_print_set_noln(VayuList* s);

void vayu_print_list_noln(VayuList* l) {
    putchar('[');
    for (int64_t i = 0; i < l->len; ++i) {
        if (i) printf(", ");
        int8_t t = l->tags[i];
        if (t == 1) {
            printf("%s", l->items[i] ? "true" : "false");
        } else if (t == 2) {
            VayuStr* s = (VayuStr*)l->items[i];
            putchar('"');
            fwrite(s->data, 1, (size_t)s->len, stdout);
            putchar('"');
        } else if (t == 3) {
            vayu_print_list_noln((VayuList*)l->items[i]);
        } else if (t == 4) {
            vayu_print_map_noln((VayuMap*)l->items[i], 0);
        } else if (t == 5) {
            vayu_print_tuple_noln((VayuTuple*)l->items[i]);
        } else if (t == 6) {
            vayu_print_set_noln((VayuSet*)l->items[i]);
        } else if (t == 7) {
            vayu_print_float_noln(l->items[i]);
        } else {
            printf("%lld", (long long)l->items[i]);
        }
    }
    putchar(']');
}
void vayu_print_tuple_noln(VayuTuple* t) {
    putchar('(');
    for (int64_t i = 0; i < t->len; ++i) {
        if (i) printf(", ");
        int8_t tag = t->tags[i];
        if (tag == 1) {
            printf("%s", t->items[i] ? "true" : "false");
        } else if (tag == 2) {
            VayuStr* s = (VayuStr*)t->items[i];
            putchar('"');
            fwrite(s->data, 1, (size_t)s->len, stdout);
            putchar('"');
        } else if (tag == 3) {
            vayu_print_list_noln((VayuList*)t->items[i]);
        } else if (tag == 4) {
            vayu_print_map_noln((VayuMap*)t->items[i], 0);
        } else if (tag == 5) {
            vayu_print_tuple_noln((VayuTuple*)t->items[i]);
        } else if (tag == 6) {
            vayu_print_set_noln((VayuSet*)t->items[i]);
        } else if (tag == 7) {
            vayu_print_float_noln(t->items[i]);
        } else {
            printf("%lld", (long long)t->items[i]);
        }
    }
    if (t->len == 1) putchar(',');
    putchar(')');
}
void vayu_print_tuple(VayuTuple* t) { vayu_print_tuple_noln(t); putchar('\n'); }

void vayu_print_set_noln(VayuSet* s) {
    if (s->len == 0) { printf("set()"); return; }
    putchar('{');
    for (int64_t i = 0; i < s->len; ++i) {
        if (i) printf(", ");
        int8_t tag = s->tags[i];
        if (tag == 1) {
            printf("%s", s->items[i] ? "true" : "false");
        } else if (tag == 2) {
            VayuStr* str = (VayuStr*)s->items[i];
            putchar('"');
            fwrite(str->data, 1, (size_t)str->len, stdout);
            putchar('"');
        } else if (tag == 5) {
            vayu_print_tuple_noln((VayuTuple*)s->items[i]);
        } else if (tag == 6) {
            vayu_print_set_noln((VayuSet*)s->items[i]);
        } else if (tag == 7) {
            vayu_print_float_noln(s->items[i]);
        } else {
            printf("%lld", (long long)s->items[i]);
        }
    }
    putchar('}');
}
void vayu_print_set(VayuSet* s) { vayu_print_set_noln(s); putchar('\n'); }
void vayu_print_list(VayuList* l, int64_t ek) {
    (void)ek;
    vayu_print_list_noln(l); putchar('\n');
}
void vayu_print_map_noln(VayuMap* m, int64_t vk) {
    putchar('{');
    int64_t printed = 0;
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        if (printed) printf(", ");
        putchar('"');
        fwrite(m->entries[i].key->data, 1, (size_t)m->entries[i].key->len, stdout);
        printf("\": ");
        if (vk == 2) putchar('"');
        vayu_print_value(m->entries[i].value, vk);
        if (vk == 2) putchar('"');
        printed++;
    }
    putchar('}');
}
void vayu_print_map(VayuMap* m, int64_t vk) {
    vayu_print_map_noln(m, vk); putchar('\n');
}

long long vayu_floordiv(long long a, long long b) {
    if (b == 0) {
        vayu_raise_str(vayu_mkstr_c("ZeroDivisionError"),
                       vayu_mkstr_c("division by zero"));
    }
    long long q = a / b;
    if ((a ^ b) < 0 && q * b != a) q--;
    return q;
}
long long vayu_mod(long long a, long long b) {
    if (b == 0) {
        vayu_raise_str(vayu_mkstr_c("ZeroDivisionError"),
                       vayu_mkstr_c("modulo by zero"));
    }
    long long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
long long vayu_pow_int(long long a, long long b) {
    if (b < 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("native: ** with a negative exponent is not supported"));
    }
    long long r = 1;
    while (b > 0) {
        if (b & 1) r *= a;
        b >>= 1;
        if (b) a *= a;
    }
    return r;
}

// ---- Phase 13.1: pure helpers ----
int64_t vayu_hash_int(int64_t x) {
    uint64_t h = (uint64_t)x;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (int64_t)h;
}
int64_t vayu_hash_str(VayuStr* s) {
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < s->len; ++i) {
        h ^= (uint8_t)s->data[i];
        h *= 1099511628211ULL;
    }
    return (int64_t)h;
}
int64_t vayu_round_int(int64_t x) { return x; }
int64_t vayu_pow_mod(int64_t b, int64_t e, int64_t m) {
    if (m == 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("pow(): modulus cannot be zero"));
    }
    if (m < 0) m = -m;
    int64_t r = 1 % m;
    b %= m;
    if (b < 0) b += m;
    while (e > 0) {
        if (e & 1) r = (r * b) % m;
        b = (b * b) % m;
        e >>= 1;
    }
    return r;
}
int64_t vayu_sign_int(int64_t x) { return (x > 0) - (x < 0); }
int64_t vayu_gcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}
int64_t vayu_lcm(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    int64_t g = vayu_gcd(a, b);
    return (a / g) * b;
}
int64_t vayu_clamp_int(int64_t x, int64_t lo, int64_t hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
int64_t vayu_comb(int64_t n, int64_t k) {
    if (n < 0 || k < 0 || k > n) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("comb(): invalid arguments"));
    }
    if (k > n - k) k = n - k;
    int64_t r = 1;
    for (int64_t i = 1; i <= k; ++i) {
        r = r * (n - k + i) / i;
    }
    return r;
}

int64_t vayu_perm(int64_t n, int64_t k) {
    if (n < 0 || k < 0 || k > n) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("perm(): invalid arguments"));
    }
    int64_t r = 1;
    for (int64_t i = 0; i < k; ++i) r *= (n - i);
    return r;
}

int64_t vayu_isqrt(int64_t n) {
    if (n < 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("isqrt() of negative number"));
    }
    if (n < 2) return n;
    int64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

int64_t vayu_factorial(int64_t n) {
    if (n < 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("factorial() of negative number"));
    }
    if (n > 20) {
        vayu_raise_str(vayu_mkstr_c("OverflowError"),
                       vayu_mkstr_c("factorial() argument too large"));
    }
    int64_t r = 1;
    for (int64_t i = 2; i <= n; ++i) r *= i;
    return r;
}
int64_t vayu_list_size(VayuList* l) { return l->len; }

VayuStr* vayu_read_file(VayuStr* path) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;

    FILE* f = fopen(buf, "rb");
    if (!f) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_concat_c("cannot open file: ", path));
    }
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)sz + 1);
    s->len = sz;
    if (sz > 0) fread(s->data, 1, (size_t)sz, f);
    s->data[sz] = 0;
    fclose(f);
    return s;
}
void vayu_write_file(VayuStr* path, VayuStr* content) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;

    FILE* f = fopen(buf, "wb");
    if (!f) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_concat_c("cannot write file: ", path));
    }
    fwrite(content->data, 1, (size_t)content->len, f);
    fclose(f);
}
int64_t vayu_file_exists(VayuStr* path) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;
    FILE* f = fopen(buf, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

VayuList* vayu_get_args(void) {
    VayuList* l = vayu_list_new();
    for (int i = 1; i < g_argc; ++i) {
        vayu_list_push(l, (int64_t)vayu_mkstr_c(g_argv[i]));
    }
    return l;
}
int64_t vayu_run_command(VayuStr* cmd) {
    char buf[8192];
    int64_t n = cmd->len < 8191 ? cmd->len : 8191;
    memcpy(buf, cmd->data, (size_t)n);
    buf[n] = 0;
#ifdef _WIN32
    {
        char wrapped[8256];
        int wr = snprintf(wrapped, sizeof(wrapped), "call %s", buf);
        if (wr > 0) return (int64_t)system(wrapped);
    }
#endif
    return (int64_t)system(buf);
}
void vayu_exit(int64_t code) { exit((int)code); }

// ---- gui (Phase 19.1-19.3) ----
#ifdef _WIN32

typedef void (*vayu_gui_cb_t)(int64_t);

#define VAYU_EV_NONE        0
#define VAYU_EV_PAINT       1
#define VAYU_EV_MOUSE_DOWN  2
#define VAYU_EV_MOUSE_UP    3
#define VAYU_EV_MOUSE_MOVE  4
#define VAYU_EV_KEY_DOWN    5
#define VAYU_EV_KEY_UP      6
#define VAYU_EV_CLOSE       7
#define VAYU_EV_TIMER       8
#define VAYU_EV_MOUSE_WHEEL 9
#define VAYU_EV_RESIZE      10
#define VAYU_EV_FOCUS_IN    11
#define VAYU_EV_FOCUS_OUT   12
#define VAYU_EV_ENTER       13
#define VAYU_EV_CONTROL     14

#define VAYU_WM_ENTER       (WM_APP + 1)

typedef struct {
    HWND    hwnd;
    HDC     mem_dc;
    HBITMAP bmp;
    HBITMAP old_bmp;
    int     bmp_w, bmp_h;
    HDC     hdc;
    int     in_paint;
    int     mx, my;
    int     last_key;
    int     wheel_delta;
    int     win_w, win_h;
    int     focus_in;
} VayuGuiState;

static VayuGuiState g_gui;
static vayu_gui_cb_t g_gui_cb = NULL;
static WNDPROC g_edit_old_proc = NULL;

/* Phase 20.1 — GDI+ token and readiness flag.  Set once in vayu_gui_init,
   cleared in WM_DESTROY.  All 20.x functions short-circuit if not ready. */
static ULONG_PTR g_gdiplus_token = 0;
static int       g_gdiplus_ready = 0;

/* ---------------- UI-startup caches (Phase 22 perf) ------------------ */

/* Single cached GpGraphics for the live window back buffer.  Invalidated
   whenever the back buffer is recreated (i.e. on resize). */
static GpGraphics* g_win_gfx       = NULL;
static int         g_win_gfx_w     = 0;
static int         g_win_gfx_h     = 0;

/* Font cache: name-slice + size + weight + italic -> handle.  16 slots,
   linear scan.  Handles stored as void* to avoid forward-declaring
   VayuFont, which lives further down the file. */
typedef struct { char key[96]; void* f; } VayuFontCacheEntry;
static VayuFontCacheEntry g_font_cache[16];
static int                g_font_cache_n = 0;

/* Pen cache: (argb, width) -> GpPen*.  32 slots, round-robin eviction. */
typedef struct { ARGB c; REAL w; GpPen* p; } VayuPenCacheEntry;
static VayuPenCacheEntry g_pen_cache[32];
static int               g_pen_cache_n = 0;
static int               g_pen_cache_cursor = 0;

static LRESULT CALLBACK vayu_gui_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        HWND parent = GetParent(h);
        if (parent) PostMessageA(parent, VAYU_WM_ENTER, 0, 0);
        return 0;
    }
    if (g_edit_old_proc) return CallWindowProcA(g_edit_old_proc, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}

static LRESULT CALLBACK vayu_gui_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        if (g_gui_cb) g_gui_cb(VAYU_EV_CLOSE);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_gui.bmp) {
            SelectObject(g_gui.mem_dc, g_gui.old_bmp);
            DeleteObject(g_gui.bmp);
            g_gui.bmp = NULL;
        }
        for (int i = 0; i < g_pen_cache_n; ++i) {
            if (g_pen_cache[i].p) GdipDeletePen(g_pen_cache[i].p);
            g_pen_cache[i].p = NULL;
        }
        g_pen_cache_n = 0;
        if (g_win_gfx) { GdipDeleteGraphics(g_win_gfx); g_win_gfx = NULL; }
        if (g_gdiplus_ready) {
            GdiplusShutdown(g_gdiplus_token);
            g_gdiplus_ready = 0;
        }
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        g_gui.win_w = (int)LOWORD(lp);
        g_gui.win_h = (int)HIWORD(lp);
        if (g_gui_cb && g_gui.win_w > 0 && g_gui.win_h > 0) g_gui_cb(VAYU_EV_RESIZE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC win = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        int cw = cr.right > 0 ? cr.right : 1;
        int ch = cr.bottom > 0 ? cr.bottom : 1;
        g_gui.win_w = cw; g_gui.win_h = ch;

        if (!g_gui.mem_dc) {
            HDC screen = GetDC(NULL);
            g_gui.mem_dc = CreateCompatibleDC(screen);
            ReleaseDC(NULL, screen);
        }
        if (!g_gui.bmp || g_gui.bmp_w != cw || g_gui.bmp_h != ch) {
            if (g_gui.bmp) {
                SelectObject(g_gui.mem_dc, g_gui.old_bmp);
                DeleteObject(g_gui.bmp);
            }
            g_gui.bmp = CreateCompatibleBitmap(win, cw, ch);
            g_gui.old_bmp = (HBITMAP)SelectObject(g_gui.mem_dc, g_gui.bmp);
            g_gui.bmp_w = cw; g_gui.bmp_h = ch;
            /* Cached window GpGraphics points at the old HDC target. */
            if (g_win_gfx) { GdipDeleteGraphics(g_win_gfx); g_win_gfx = NULL; }
            g_win_gfx_w = 0; g_win_gfx_h = 0;
        }

        g_gui.hdc = g_gui.mem_dc;
        g_gui.in_paint = 1;
        if (g_gui_cb) g_gui_cb(VAYU_EV_PAINT);
        g_gui.in_paint = 0;

        BitBlt(win, 0, 0, cw, ch, g_gui.mem_dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_gui.mx = (int)(short)LOWORD(lp);
        g_gui.my = (int)(short)HIWORD(lp);
        if (g_gui_cb) g_gui_cb(VAYU_EV_MOUSE_DOWN);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        g_gui.mx = (int)(short)LOWORD(lp);
        g_gui.my = (int)(short)HIWORD(lp);
        if (g_gui_cb) g_gui_cb(VAYU_EV_MOUSE_UP);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        g_gui.mx = (int)(short)LOWORD(lp);
        g_gui.my = (int)(short)HIWORD(lp);
        if (g_gui_cb) g_gui_cb(VAYU_EV_MOUSE_MOVE);
        return 0;
    case WM_MOUSEWHEEL:
        g_gui.wheel_delta = (int)(short)HIWORD(wp);
        if (g_gui_cb) g_gui_cb(VAYU_EV_MOUSE_WHEEL);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        g_gui.last_key = (int)wp;
        if (g_gui_cb) g_gui_cb(VAYU_EV_KEY_DOWN);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYUP:
        g_gui.last_key = (int)wp;
        if (g_gui_cb) g_gui_cb(VAYU_EV_KEY_UP);
        return 0;
    case WM_SETFOCUS:
        g_gui.focus_in = 1;
        if (g_gui_cb) g_gui_cb(VAYU_EV_FOCUS_IN);
        return 0;
    case WM_KILLFOCUS:
        g_gui.focus_in = 0;
        if (g_gui_cb) g_gui_cb(VAYU_EV_FOCUS_OUT);
        return 0;
    case WM_TIMER:
        if (g_gui_cb) g_gui_cb(VAYU_EV_TIMER);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_VSCROLL:
        if (g_gui_cb) g_gui_cb(VAYU_EV_CONTROL);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case VAYU_WM_ENTER:
        if (g_gui_cb) g_gui_cb(VAYU_EV_ENTER);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int64_t vayu_gui_init(void) {
    static int done = 0;
    if (done) return 1;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    /* Phase 20.1 — one-time GDI+ startup.  Must precede any Gdip* call. */
    if (!g_gdiplus_ready) {
        GdiplusStartupInput gsi;
        gsi.GdiplusVersion           = 1;
        gsi.DebugEventCallback       = NULL;
        gsi.SuppressBackgroundThread = FALSE;
        gsi.SuppressExternalCodecs   = FALSE;
        if (GdiplusStartup(&g_gdiplus_token, &gsi, NULL) == 0) {
            g_gdiplus_ready = 1;
        } else {
            g_gdiplus_ready = 0;
        }
    }

    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = vayu_gui_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "VayuWindow";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassA(&wc)) {
        if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) { done = 1; return 1; }
        return 0;
    }
    done = 1; return 1;
}

int64_t vayu_gui_create_window(VayuStr* title, int64_t w, int64_t h) {
    if (!vayu_gui_init()) return 0;
    HWND hwnd = CreateWindowExA(0, "VayuWindow", title->data,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        (int)w, (int)h, NULL, NULL, GetModuleHandleA(NULL), NULL);
    memset(&g_gui, 0, sizeof(g_gui));
    g_gui.hwnd = hwnd; g_gui.last_key = -1;
    g_gui.win_w = (int)w; g_gui.win_h = (int)h;
    return (int64_t)hwnd;
}
void vayu_gui_show_window(int64_t h) {
    HWND hwnd = (HWND)h; if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
}
void vayu_gui_invalidate(void) { if (g_gui.hwnd) InvalidateRect(g_gui.hwnd, NULL, FALSE); }
void vayu_gui_set_title(int64_t h, VayuStr* t) { HWND w=(HWND)h; if(w) SetWindowTextA(w,t->data); }
void vayu_gui_set_size(int64_t h, int64_t w, int64_t k) { HWND hw=(HWND)h; if(hw) SetWindowPos(hw,NULL,0,0,(int)w,(int)k,SWP_NOMOVE|SWP_NOZORDER); }
void vayu_gui_set_callback(int64_t fp) { g_gui_cb = (vayu_gui_cb_t)fp; vayu_gui_invalidate(); }
int64_t vayu_gui_mouse_x(void) { return g_gui.mx; }
int64_t vayu_gui_mouse_y(void) { return g_gui.my; }
int64_t vayu_gui_last_key(void) { return g_gui.last_key; }
int64_t vayu_gui_wheel_delta(void) { return g_gui.wheel_delta; }
int64_t vayu_gui_window_w(void) { return g_gui.win_w; }
int64_t vayu_gui_window_h(void) { return g_gui.win_h; }
void vayu_gui_set_timer(int64_t ms) { if (g_gui.hwnd && ms > 0) SetTimer(g_gui.hwnd, 1, (UINT)ms, NULL); }
void vayu_gui_clear_timer(void) { if (g_gui.hwnd) KillTimer(g_gui.hwnd, 1); }
int64_t vayu_gui_run(int64_t h) { (void)h; MSG m; while (GetMessageA(&m,NULL,0,0)>0){TranslateMessage(&m);DispatchMessageA(&m);} return 0; }
void vayu_gui_close(int64_t h) { HWND w=(HWND)h; if(w) DestroyWindow(w); }
void vayu_gui_quit(void) { PostQuitMessage(0); }

static COLORREF vayu_gui_rgb(int64_t c) {
    return RGB((int)((c>>16)&0xFF), (int)((c>>8)&0xFF), (int)(c&0xFF));
}
void vayu_gui_clear(int64_t color) {
    if (!g_gui.in_paint) return;
    RECT r; r.left=0; r.top=0; r.right=g_gui.bmp_w; r.bottom=g_gui.bmp_h;
    HBRUSH br = CreateSolidBrush(vayu_gui_rgb(color));
    FillRect(g_gui.hdc, &r, br); DeleteObject(br);
}
void vayu_gui_fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    if (!g_gui.in_paint) return;
    RECT r; r.left=(LONG)x; r.top=(LONG)y; r.right=(LONG)(x+w); r.bottom=(LONG)(y+h);
    HBRUSH br = CreateSolidBrush(vayu_gui_rgb(color));
    FillRect(g_gui.hdc, &r, br); DeleteObject(br);
}
void vayu_gui_outline_rect(int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    if (!g_gui.in_paint) return;
    HPEN pen = CreatePen(PS_SOLID, 1, vayu_gui_rgb(color));
    HGDIOBJ op = SelectObject(g_gui.hdc, pen);
    HGDIOBJ ob = SelectObject(g_gui.hdc, GetStockObject(NULL_BRUSH));
    Rectangle(g_gui.hdc, (int)x, (int)y, (int)(x+w), (int)(y+h));
    SelectObject(g_gui.hdc, ob); SelectObject(g_gui.hdc, op); DeleteObject(pen);
}
void vayu_gui_line(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
    if (!g_gui.in_paint) return;
    HPEN pen = CreatePen(PS_SOLID, 1, vayu_gui_rgb(color));
    HGDIOBJ op = SelectObject(g_gui.hdc, pen);
    MoveToEx(g_gui.hdc, (int)x1, (int)y1, NULL);
    LineTo(g_gui.hdc, (int)x2, (int)y2);
    SelectObject(g_gui.hdc, op); DeleteObject(pen);
}
void vayu_gui_text(int64_t sp, int64_t x, int64_t y, int64_t color) {
    if (!g_gui.in_paint) return;
    VayuStr* s = (VayuStr*)sp;
    SetTextColor(g_gui.hdc, vayu_gui_rgb(color));
    SetBkMode(g_gui.hdc, TRANSPARENT);
    TextOutA(g_gui.hdc, (int)x, (int)y, s->data, (int)s->len);
}
int64_t vayu_gui_rect_hit(int64_t x, int64_t y, int64_t w, int64_t h, int64_t px, int64_t py) {
    return (px >= x && px < x+w && py >= y && py < y+h) ? 1 : 0;
}

/* ===========================================================================
 * Phase 20.1 - GDI+ canvas, antialiased shapes, sized text.
 * ========================================================================= */

typedef struct VayuCanvas {
    GpGraphics* g;        /* current GDI+ drawing context            */
    GpBitmap*   bmp;      /* non-NULL iff offscreen                  */
    int         w, h;
    int         is_window;/* 1 if backed by the live window buffer   */
    REAL        line_width;
    GpLineCap   line_cap;
    GpLineJoin  line_join;
    /* Phase 20.3 - transform / clip state */
    GraphicsState state_stack[32];
    int           state_sp;
    int           fill_mode;   /* 0=alternate, 1=winding */
    int           composite;   /* 0=source-over, 1=source-copy */
    /* Phase 20.4 - text alignment applied to text_ex. */
    int           text_align;  /* 0=left, 1=center, 2=right */
    /* Phase 22 opt - cached window GpGraphics.  is_window canvases reuse
       this so we don't create a fresh GDI+ object every WM_PAINT. */
    int           owns_g;      /* 1 if we must DeleteGraphics on free */
} VayuCanvas;

typedef struct VayuFont {
    GpFontFamily* fam;
    GpFont*       font;
} VayuFont;

static VayuCanvas* vayu_canvas_alloc(void) {
    VayuCanvas* c = (VayuCanvas*)malloc(sizeof(VayuCanvas));
    c->g = NULL; c->bmp = NULL;
    c->w = 0; c->h = 0; c->is_window = 0;
    c->line_width = 1.0f;
    c->line_cap   = LineCapFlat;
    c->line_join  = LineJoinMiter;
    c->state_sp   = 0;
    c->fill_mode  = 1;   /* winding, matches prior polygon behaviour */
    c->composite  = 0;   /* source-over */
    c->text_align = 0;   /* left */
    c->owns_g     = 1;   /* default; window canvases flip this to 0 */
    return c;
}

static void vayu_canvas_destroy(VayuCanvas* c) {
    if (!c) return;
    if (c->g && c->owns_g) { GdipDeleteGraphics(c->g); }
    c->g = NULL;
    if (c->bmp) { GdipDisposeImage((GpImage*)c->bmp); c->bmp = NULL; }
    free(c);
}

static GpSolidFill* vayu_solid(ARGB color) {
    GpSolidFill* b = NULL;
    GdipCreateSolidFill(color, &b);
    return b;
}

static GpPen* vayu_canvas_make_pen(VayuCanvas* c, ARGB color) {
    /* Cache hit: identical (color, width, cap, join) triple. */
    for (int i = 0; i < g_pen_cache_n; ++i) {
        VayuPenCacheEntry* e = &g_pen_cache[i];
        if (e->c == color && e->w == c->line_width) return e->p;
    }
    GpPen* p = NULL;
    GdipCreatePen1(color, c->line_width, UnitPixel, &p);
    GdipSetPenLineCap197819(p, c->line_cap, c->line_cap, DashCapFlat);
    GdipSetPenLineJoin(p, c->line_join);

    if (g_pen_cache_n < 32) {
        g_pen_cache[g_pen_cache_n].c = color;
        g_pen_cache[g_pen_cache_n].w = c->line_width;
        g_pen_cache[g_pen_cache_n].p = p;
        g_pen_cache_n = g_pen_cache_n + 1;
    } else {
        int idx = g_pen_cache_cursor;
        if (g_pen_cache[idx].p) GdipDeletePen(g_pen_cache[idx].p);
        g_pen_cache[idx].c = color;
        g_pen_cache[idx].w = c->line_width;
        g_pen_cache[idx].p = p;
        g_pen_cache_cursor = (g_pen_cache_cursor + 1) % 32;
    }
    return p;
}

int64_t vayu_gui_canvas_new(int64_t w, int64_t h) {
    if (!g_gdiplus_ready) return 0;
    VayuCanvas* c = vayu_canvas_alloc();
    c->w = (int)w; c->h = (int)h;
    GdipCreateBitmapFromScan0((INT)w, (INT)h, 0,
                              PixelFormat32bppARGB, NULL, &c->bmp);
    if (!c->bmp) { free(c); return 0; }
    GdipGetImageGraphicsContext((GpImage*)c->bmp, &c->g);
    GdipSetSmoothingMode(c->g, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(c->g, TextRenderingHintAntiAlias);
    GdipSetPixelOffsetMode(c->g, PixelOffsetModeHalf);
    return (int64_t)c;
}

int64_t vayu_gui_canvas_from_window(void) {
    if (!g_gdiplus_ready) return 0;
    if (!g_gui.mem_dc) return 0;
    /* Reuse the cached GpGraphics when the back buffer hasn't changed.
       This is the single biggest startup/frame win: we skip one GDI+
       object construction per WM_PAINT. */
    if (g_win_gfx &&
        g_win_gfx_w == g_gui.bmp_w && g_win_gfx_h == g_gui.bmp_h) {
        VayuCanvas* c = vayu_canvas_alloc();
        c->w = g_gui.bmp_w; c->h = g_gui.bmp_h;
        c->is_window = 1;
        c->owns_g    = 0;
        c->g         = g_win_gfx;
        return (int64_t)c;
    }
    if (g_win_gfx) { GdipDeleteGraphics(g_win_gfx); g_win_gfx = NULL; }
    GpGraphics* g = NULL;
    GdipCreateFromHDC(g_gui.mem_dc, &g);
    if (!g) return 0;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    g_win_gfx   = g;
    g_win_gfx_w = g_gui.bmp_w;
    g_win_gfx_h = g_gui.bmp_h;
    VayuCanvas* c = vayu_canvas_alloc();
    c->w = g_gui.bmp_w; c->h = g_gui.bmp_h;
    c->is_window = 1;
    c->owns_g    = 0;
    c->g         = g_win_gfx;
    return (int64_t)c;
}

void vayu_gui_canvas_free(int64_t h) {
    if (!h) return;
    VayuCanvas* c = (VayuCanvas*)h;
    /* Window canvases share the cached GpGraphics; just drop the wrapper. */
    if (c->is_window && !c->owns_g) {
        if (c->bmp) { GdipDisposeImage((GpImage*)c->bmp); c->bmp = NULL; }
        free(c);
        return;
    }
    vayu_canvas_destroy(c);
}

void vayu_gui_canvas_clear(int64_t h, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GdipGraphicsClear(c->g, (ARGB)argb);
}

void vayu_gui_canvas_fill_rect(int64_t h, int64_t x, int64_t y,
                               int64_t w, int64_t k, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpSolidFill* b = vayu_solid((ARGB)argb);
    GdipFillRectangleI(c->g, (GpBrush*)b, (INT)x, (INT)y, (INT)w, (INT)k);
    GdipDeleteBrush((GpBrush*)b);
}

void vayu_gui_canvas_outline_rect(int64_t h, int64_t x, int64_t y,
                                  int64_t w, int64_t k,
                                  int64_t argb, int64_t thickness) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpPen* p = NULL;
    GdipCreatePen1((ARGB)argb, (REAL)thickness, UnitPixel, &p);
    GdipDrawRectangleI(c->g, p, (INT)x, (INT)y, (INT)w, (INT)k);
    (void)p;
}

void vayu_gui_blit_canvas(int64_t src, int64_t x, int64_t y) {
    VayuCanvas* c = (VayuCanvas*)src;
    if (!c || !c->bmp) return;
    if (!g_gui.mem_dc) return;
    GpGraphics* wg = NULL;
    GdipCreateFromHDC(g_gui.mem_dc, &wg);
    if (!wg) return;
    GdipDrawImageI(wg, (GpImage*)c->bmp, (INT)x, (INT)y);
    GdipDeleteGraphics(wg);
}

void vayu_gui_circle(int64_t h, int64_t cx, int64_t cy, int64_t r, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpSolidFill* b = vayu_solid((ARGB)argb);
    GdipFillEllipse(c->g, (GpBrush*)b,
                    (REAL)(cx - r), (REAL)(cy - r),
                    (REAL)(2 * r),  (REAL)(2 * r));
    GdipDeleteBrush((GpBrush*)b);
}

void vayu_gui_circle_outline(int64_t h, int64_t cx, int64_t cy,
                             int64_t r, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpPen* p = vayu_canvas_make_pen(c, (ARGB)argb);
    GdipDrawEllipse(c->g, p,
                    (REAL)(cx - r), (REAL)(cy - r),
                    (REAL)(2 * r),  (REAL)(2 * r));
    (void)p;
}

void vayu_gui_ellipse(int64_t h, int64_t cx, int64_t cy,
                      int64_t rx, int64_t ry, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpSolidFill* b = vayu_solid((ARGB)argb);
    GdipFillEllipse(c->g, (GpBrush*)b,
                    (REAL)(cx - rx), (REAL)(cy - ry),
                    (REAL)(2 * rx),  (REAL)(2 * ry));
    GdipDeleteBrush((GpBrush*)b);
}

void vayu_gui_ellipse_outline(int64_t h, int64_t cx, int64_t cy,
                              int64_t rx, int64_t ry, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpPen* p = vayu_canvas_make_pen(c, (ARGB)argb);
    GdipDrawEllipse(c->g, p,
                    (REAL)(cx - rx), (REAL)(cy - ry),
                    (REAL)(2 * rx),  (REAL)(2 * ry));
    (void)p;
}

/* GDI+ has no native rounded rect; compose from 4 arcs + 2 rects. */
static void vayu_draw_rounded_path(GpGraphics* g, GpPen* pen, GpBrush* br,
                                   REAL x, REAL y, REAL w, REAL k, REAL r) {
    GpPath* path = NULL;
    GdipCreatePath(FillModeWinding, &path);
    if (r < 0)   r = 0;
    if (r > w/2) r = w/2;
    if (r > k/2) r = k/2;
    GdipAddPathArc(path, x,         y,         r*2, r*2, 180.0f, 90.0f);
    GdipAddPathArc(path, x+w-r*2,   y,         r*2, r*2, 270.0f, 90.0f);
    GdipAddPathArc(path, x+w-r*2,   y+k-r*2,   r*2, r*2,   0.0f, 90.0f);
    GdipAddPathArc(path, x,         y+k-r*2,   r*2, r*2,  90.0f, 90.0f);
    GdipClosePathFigure(path);
    if (br)  GdipFillPath(g, br, path);
    if (pen) GdipDrawPath(g, pen, path);
    GdipDeletePath(path);
}

void vayu_gui_rounded_rect(int64_t h, int64_t x, int64_t y,
                           int64_t w, int64_t k, int64_t radius,
                           int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpSolidFill* b = vayu_solid((ARGB)argb);
    vayu_draw_rounded_path(c->g, NULL, (GpBrush*)b,
                           (REAL)x, (REAL)y, (REAL)w, (REAL)k, (REAL)radius);
    GdipDeleteBrush((GpBrush*)b);
}

void vayu_gui_rounded_rect_outline(int64_t h, int64_t x, int64_t y,
                                   int64_t w, int64_t k, int64_t radius,
                                   int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpPen* p = vayu_canvas_make_pen(c, (ARGB)argb);
    vayu_draw_rounded_path(c->g, p, NULL,
                           (REAL)x, (REAL)y, (REAL)w, (REAL)k, (REAL)radius);
    (void)p;
}

void vayu_gui_arc(int64_t h, int64_t cx, int64_t cy, int64_t r,
                  int64_t start_deg, int64_t sweep_deg, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GpPen* p = vayu_canvas_make_pen(c, (ARGB)argb);
    GdipDrawArc(c->g, p,
                (REAL)(cx - r), (REAL)(cy - r),
                (REAL)(2 * r),  (REAL)(2 * r),
                (REAL)start_deg, (REAL)sweep_deg);
    (void)p;
}

/* polygon takes a Vayu list of [x0, y0, x1, y1, ...] pairs. */
void vayu_gui_polygon(int64_t h, int64_t pts, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    VayuList* lst = (VayuList*)pts;
    if (!lst || lst->len < 6) return;
    int npts = (int)(lst->len / 2);
    GpPointF* pf = (GpPointF*)malloc(sizeof(GpPointF) * (size_t)npts);
    for (int i = 0; i < npts; ++i) {
        pf[i].X = (REAL)lst->items[i * 2];
        pf[i].Y = (REAL)lst->items[i * 2 + 1];
    }
    GpSolidFill* b = vayu_solid((ARGB)argb);
    GpFillMode fm = (c->fill_mode == 1) ? FillModeWinding : FillModeAlternate;
    GdipFillPolygon(c->g, (GpBrush*)b, pf, npts, fm);
    GdipDeleteBrush((GpBrush*)b);
    free(pf);
}

void vayu_gui_polygon_outline(int64_t h, int64_t pts, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    VayuList* lst = (VayuList*)pts;
    if (!lst || lst->len < 6) return;
    int npts = (int)(lst->len / 2);
    GpPointF* pf = (GpPointF*)malloc(sizeof(GpPointF) * (size_t)npts);
    for (int i = 0; i < npts; ++i) {
        pf[i].X = (REAL)lst->items[i * 2];
        pf[i].Y = (REAL)lst->items[i * 2 + 1];
    }
    GpPen* p = vayu_canvas_make_pen(c, (ARGB)argb);
    GdipDrawPolygon(c->g, p, pf, npts);
    (void)p;
    free(pf);
}

void vayu_gui_line_width(int64_t h, int64_t px) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c) return;
    if (px < 1) px = 1;
    c->line_width = (REAL)px;
}

void vayu_gui_line_cap(int64_t h, int64_t mode) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c) return;
    if (mode == 1)      c->line_cap = LineCapSquare;
    else if (mode == 2) c->line_cap = LineCapRound;
    else                c->line_cap = LineCapFlat;
}

void vayu_gui_line_join(int64_t h, int64_t mode) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c) return;
    if (mode == 1)      c->line_join = LineJoinBevel;
    else if (mode == 2) c->line_join = LineJoinRound;
    else                c->line_join = LineJoinMiter;
}

int64_t vayu_gui_font_new(int64_t name_sp, int64_t size, int64_t weight, int64_t italic) {
    if (!g_gdiplus_ready) return 0;
    VayuStr* s = (VayuStr*)name_sp;

    /* Cache key: name + size + weight + italic.  Linear scan of 16 slots. */
    char key[96];
    int kl = (int)s->len;
    if (kl > 80) kl = 80;
    memcpy(key, s->data, (size_t)kl);
    int kn = snprintf(key + kl, 16, "|%lld|%lld|%lld",
                      (long long)size, (long long)weight, (long long)italic);
    (void)kn;
    for (int i = 0; i < g_font_cache_n; ++i) {
        if (strcmp(g_font_cache[i].key, key) == 0)
            return (int64_t)g_font_cache[i].f;
    }

    WCHAR wname[128];
    int wl = MultiByteToWideChar(CP_UTF8, 0, s->data, (int)s->len, wname, 127);
    if (wl < 0) wl = 0;
    wname[wl] = 0;
    VayuFont* f = (VayuFont*)malloc(sizeof(VayuFont));
    f->fam = NULL;
    f->font = NULL;
    GdipCreateFontFamilyFromName(wname, NULL, &f->fam);
    if (!f->fam) {
        GdipCreateFontFamilyFromName(L"Arial", NULL, &f->fam);
    }
    INT style = FontStyleRegular;
    if (weight >= 700) style = FontStyleBold;
    if (italic) {
        if (style == FontStyleBold) style = FontStyleBoldItalic;
        else                        style = FontStyleItalic;
    }
    if (size < 4) size = 4;
    GdipCreateFont(f->fam, (REAL)size, style, UnitPixel, &f->font);

    if (g_font_cache_n < 16) {
        memcpy(g_font_cache[g_font_cache_n].key, key, sizeof(key));
        g_font_cache[g_font_cache_n].f = (void*)f;
        g_font_cache_n = g_font_cache_n + 1;
    }
    return (int64_t)f;
}

void vayu_gui_font_free(int64_t h) {
    /* Fonts live for the process lifetime — cached, shared, freed only
       at GdiplusShutdown.  Demos call font_free at teardown and we
       deliberately ignore it so the cache stays valid. */
    (void)h;
}

int64_t vayu_gui_font_default(void) {
    VayuStr* n = vayu_mkstr_c("Segoe UI");
    int64_t h = vayu_gui_font_new((int64_t)n, 14, 400, 0);
    return h;
}

void vayu_gui_text_ex(int64_t canvas_h, int64_t font_h, int64_t str_sp,
                      int64_t x, int64_t y, int64_t argb) {
    VayuCanvas* c = (VayuCanvas*)canvas_h;
    VayuFont*   f = (VayuFont*)font_h;
    if (!c || !c->g || !f || !f->font) return;
    VayuStr* s = (VayuStr*)str_sp;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s->data, (int)s->len, NULL, 0);
    if (wlen < 0) wlen = 0;
    WCHAR* w = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s->data, (int)s->len, w, wlen);
    w[wlen] = 0;
    GpSolidFill* b = vayu_solid((ARGB)argb);
    RectF rf;
    rf.X = (REAL)x; rf.Y = (REAL)y;
    rf.Width = 100000.0f; rf.Height = 100000.0f;
    if (c->text_align != 0) {
        RectF bounds;
        memset(&bounds, 0, sizeof(bounds));
        GdipMeasureString(c->g, w, wlen, f->font, &rf, NULL, &bounds, NULL, NULL);
        if (c->text_align == 1)       rf.X = (REAL)x - bounds.Width * 0.5f;
        else /* == 2, right */        rf.X = (REAL)x - bounds.Width;
    }
    GdipDrawString(c->g, w, wlen, f->font, &rf, NULL, (GpBrush*)b);
    GdipDeleteBrush((GpBrush*)b);
    free(w);
}

/* ===========================================================================
 * Phase 20.4 - text metrics + alignment.
 * ========================================================================= */

void vayu_gui_text_align(int64_t h, int64_t mode) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c) return;
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    c->text_align = (int)mode;
}

static int64_t vayu_text_measure_common(int64_t h, int64_t fh,
                                        int64_t str_sp, int want_h) {
    VayuCanvas* c = (VayuCanvas*)h;
    VayuFont*   f = (VayuFont*)fh;
    if (!c || !c->g || !f || !f->font) return 0;
    VayuStr* s = (VayuStr*)str_sp;
    if (s->len == 0) return 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s->data, (int)s->len, NULL, 0);
    if (wlen < 0) wlen = 0;
    WCHAR* w = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s->data, (int)s->len, w, wlen);
    w[wlen] = 0;
    RectF layout;
    layout.X = 0; layout.Y = 0;
    layout.Width = 100000.0f; layout.Height = 100000.0f;
    RectF bounds;
    memset(&bounds, 0, sizeof(bounds));
    GdipMeasureString(c->g, w, wlen, f->font, &layout, NULL, &bounds, NULL, NULL);
    free(w);
    if (want_h) return (int64_t)(bounds.Height + 0.5f);
    return (int64_t)(bounds.Width + 0.5f);
}

int64_t vayu_gui_text_width(int64_t h, int64_t fh, int64_t str_sp) {
    return vayu_text_measure_common(h, fh, str_sp, 0);
}

int64_t vayu_gui_text_height(int64_t h, int64_t fh, int64_t str_sp) {
    return vayu_text_measure_common(h, fh, str_sp, 1);
}

int64_t vayu_gui_font_height(int64_t fh) {
    VayuFont* f = (VayuFont*)fh;
    if (!f || !f->font) return 0;
    REAL h = 0;
    GdipGetFontHeight(f->font, NULL, &h);
    return (int64_t)(h + 0.5f);
}

int64_t vayu_gui_font_line_spacing(int64_t fh) {
    VayuFont* f = (VayuFont*)fh;
    if (!f || !f->font) return 0;
    REAL h = 0;
    GdipGetFontHeight(f->font, NULL, &h);
    /* GDI+ exposes no line-spacing call. 1.2x height approximates the
       standard text-line advance for the default face. */
    return (int64_t)(h * 1.2f + 0.5f);
}

/* ===========================================================================
 * Phase 20.2 - Bitmap I/O via GDI+'s built-in codec dispatch.
 * GDIPlus decodes BMP/PNG/JPG/GIF/TIFF without extra deps.
 * ========================================================================= */

typedef struct VayuBitmap {
    GpImage* img;
} VayuBitmap;

/* Phase 20.5 - WIC helpers (defined further down).  Returns a GpBitmap
   on success, NULL on failure.  Caller takes ownership. */
static GpBitmap* vayu_wic_load_bitmap(const char* utf8, int len);
static int       vayu_wic_save_bitmap_png(GpBitmap* bmp, int w, int h,
                                          const char* utf8, int len);

static WCHAR* vayu_utf8_to_w(const char* s, int len) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    if (wlen < 0) wlen = 0;
    WCHAR* w = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, len, w, wlen);
    w[wlen] = 0;
    return w;
}

int64_t vayu_gui_bitmap_load(int64_t path_sp) {
    if (!g_gdiplus_ready) return 0;
    VayuStr* s = (VayuStr*)path_sp;

    /* Phase 20.5 - try WIC first (reliable codec path on MinGW). */
    GpBitmap* wicbmp = vayu_wic_load_bitmap(s->data, (int)s->len);
    if (wicbmp) {
        VayuBitmap* b = (VayuBitmap*)malloc(sizeof(VayuBitmap));
        b->img = (GpImage*)wicbmp;
        return (int64_t)b;
    }

    /* Fallback: GDI+ codec (may fail on some MinGW builds). */
    WCHAR* w = vayu_utf8_to_w(s->data, (int)s->len);
    GpImage* img = NULL;
    GdipLoadImageFromFile(w, &img);
    free(w);
    if (!img) return 0;
    VayuBitmap* b = (VayuBitmap*)malloc(sizeof(VayuBitmap));
    b->img = img;
    return (int64_t)b;
}

void vayu_gui_bitmap_free(int64_t h) {
    VayuBitmap* b = (VayuBitmap*)h;
    if (!b) return;
    if (b->img) GdipDisposeImage(b->img);
    free(b);
}

int64_t vayu_gui_bitmap_width(int64_t h) {
    VayuBitmap* b = (VayuBitmap*)h;
    if (!b || !b->img) return 0;
    UINT w = 0;
    GdipGetImageWidth(b->img, &w);
    return (int64_t)w;
}

int64_t vayu_gui_bitmap_height(int64_t h) {
    VayuBitmap* b = (VayuBitmap*)h;
    if (!b || !b->img) return 0;
    UINT k = 0;
    GdipGetImageHeight(b->img, &k);
    return (int64_t)k;
}

void vayu_gui_draw_bitmap(int64_t canvas_h, int64_t bmp_h,
                          int64_t x, int64_t y) {
    VayuCanvas* c = (VayuCanvas*)canvas_h;
    VayuBitmap* b = (VayuBitmap*)bmp_h;
    if (!c || !c->g || !b || !b->img) return;
    GdipDrawImageI(c->g, b->img, (INT)x, (INT)y);
}

void vayu_gui_draw_bitmap_scaled(int64_t canvas_h, int64_t bmp_h,
                                 int64_t x, int64_t y,
                                 int64_t w, int64_t k) {
    VayuCanvas* c = (VayuCanvas*)canvas_h;
    VayuBitmap* b = (VayuBitmap*)bmp_h;
    if (!c || !c->g || !b || !b->img) return;
    GdipDrawImageRectI(c->g, b->img, (INT)x, (INT)y, (INT)w, (INT)k);
}

void vayu_gui_draw_bitmap_part(int64_t canvas_h, int64_t bmp_h,
                               int64_t sx, int64_t sy,
                               int64_t sw, int64_t sh,
                               int64_t dx, int64_t dy) {
    VayuCanvas* c = (VayuCanvas*)canvas_h;
    VayuBitmap* b = (VayuBitmap*)bmp_h;
    if (!c || !c->g || !b || !b->img) return;
    GdipDrawImageRectRectI(c->g, b->img,
                           (INT)dx, (INT)dy, (INT)sw, (INT)sh,
                           (INT)sx, (INT)sy, (INT)sw, (INT)sh,
                           UnitPixel, NULL, NULL, NULL);
}

void vayu_gui_draw_bitmap_alpha(int64_t canvas_h, int64_t bmp_h,
                                int64_t x, int64_t y, int64_t alpha) {
    VayuCanvas* c = (VayuCanvas*)canvas_h;
    VayuBitmap* b = (VayuBitmap*)bmp_h;
    if (!c || !c->g || !b || !b->img) return;
    UINT iw = 0, ih = 0;
    GdipGetImageWidth(b->img, &iw);
    GdipGetImageHeight(b->img, &ih);
    REAL a = (REAL)alpha / 255.0f;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    ColorMatrix cm = {{
        {1, 0, 0, 0, 0},
        {0, 1, 0, 0, 0},
        {0, 0, 1, 0, 0},
        {0, 0, 0, a, 0},
        {0, 0, 0, 0, 1}
    }};
    GpImageAttributes* attrs = NULL;
    GdipCreateImageAttributes(&attrs);
    GdipSetImageAttributesColorMatrix(attrs, ColorAdjustTypeDefault,
                                      TRUE, &cm, NULL,
                                      ColorMatrixFlagsDefault);
    GdipDrawImageRectRectI(c->g, b->img,
                           (INT)x, (INT)y, (INT)iw, (INT)ih,
                           0, 0, (INT)iw, (INT)ih,
                           UnitPixel, attrs, NULL, NULL);
    GdipDisposeImageAttributes(attrs);
}

static const CLSID VAYU_PNG_CLSID = {
    0x557cf406, 0x1a04, 0x11d3,
    { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e }
};

int64_t vayu_gui_canvas_save_png(int64_t canvas_h, int64_t path_sp) {
    VayuCanvas* c = (VayuCanvas*)canvas_h;
    if (!c || !c->bmp) return 0;
    VayuStr* s = (VayuStr*)path_sp;

    /* Phase 20.5 - WIC PNG encoder. */
    if (vayu_wic_save_bitmap_png(c->bmp, c->w, c->h,
                                 s->data, (int)s->len)) return 1;

    /* Fallback: GDI+ codec. */
    WCHAR* w = vayu_utf8_to_w(s->data, (int)s->len);
    GpStatus st = GdipSaveImageToFile((GpImage*)c->bmp, w,
                                      &VAYU_PNG_CLSID, NULL);
    free(w);
    return st == 0 ? 1 : 0;
}

/* Codec-independent bitmap construction.  argb_list is a flat list of
   0xAARRGGBB ints, row-major, length w*h.  Useful when we need a bitmap
   but the platform's image encoder path is unreliable. */
int64_t vayu_gui_bitmap_from_pixels(int64_t w, int64_t h, int64_t argb_list) {
    if (!g_gdiplus_ready) return 0;
    if (w <= 0 || h <= 0) return 0;
    VayuList* lst = (VayuList*)argb_list;
    if (!lst || lst->len < w * h) return 0;

    GpBitmap* bmp = NULL;
    GdipCreateBitmapFromScan0((INT)w, (INT)h, 0,
                              PixelFormat32bppARGB, NULL, &bmp);
    if (!bmp) return 0;

    GpRect rc;
    rc.X = 0; rc.Y = 0; rc.Width = (INT)w; rc.Height = (INT)h;
    BitmapData bd;
    memset(&bd, 0, sizeof(bd));
    GdipBitmapLockBits(bmp, &rc, ImageLockModeWrite,
                       PixelFormat32bppARGB, &bd);

    for (int y = 0; y < (int)h; ++y) {
        uint8_t* row = (uint8_t*)bd.Scan0 + y * bd.Stride;
        for (int x = 0; x < (int)w; ++x) {
            int64_t argb = lst->items[y * (int)w + x];
            row[x * 4 + 0] = (uint8_t)( argb        & 0xFF); /* B */
            row[x * 4 + 1] = (uint8_t)((argb >>  8) & 0xFF); /* G */
            row[x * 4 + 2] = (uint8_t)((argb >> 16) & 0xFF); /* R */
            row[x * 4 + 3] = (uint8_t)((argb >> 24) & 0xFF); /* A */
        }
    }
    GdipBitmapUnlockBits(bmp, &bd);

    VayuBitmap* b = (VayuBitmap*)malloc(sizeof(VayuBitmap));
    b->img = (GpImage*)bmp;
    return (int64_t)b;
}

/* ===========================================================================
 * Phase 20.3 - affine transforms, clipping, fill / compositing modes.
 * ========================================================================= */

void vayu_gui_push_transform(int64_t h) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    if (c->state_sp >= 32) return;
    GraphicsState st = 0;
    GdipSaveGraphics(c->g, &st);
    c->state_stack[c->state_sp++] = st;
}

void vayu_gui_pop_transform(int64_t h) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g || c->state_sp == 0) return;
    GraphicsState st = c->state_stack[--c->state_sp];
    GdipRestoreGraphics(c->g, st);
}

void vayu_gui_reset_transform(int64_t h) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GdipResetWorldTransform(c->g);
}

void vayu_gui_translate(int64_t h, int64_t dx, int64_t dy) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GdipTranslateWorldTransform(c->g, (REAL)dx, (REAL)dy, MatrixOrderPrepend);
}

void vayu_gui_rotate(int64_t h, int64_t deg) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GdipRotateWorldTransform(c->g, (REAL)deg, MatrixOrderPrepend);
}

void vayu_gui_rotate_at(int64_t h, int64_t deg, int64_t cx, int64_t cy) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GdipTranslateWorldTransform(c->g, (REAL)cx,  (REAL)cy,  MatrixOrderPrepend);
    GdipRotateWorldTransform(c->g,    (REAL)deg,            MatrixOrderPrepend);
    GdipTranslateWorldTransform(c->g, (REAL)-cx, (REAL)-cy, MatrixOrderPrepend);
}

/* scale takes integer percent: 100 = 1.0x, 50 = 0.5x, 150 = 1.5x. */
void vayu_gui_scale(int64_t h, int64_t sx_pct, int64_t sy_pct) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    REAL sx = (REAL)sx_pct / 100.0f;
    REAL sy = (REAL)sy_pct / 100.0f;
    GdipScaleWorldTransform(c->g, sx, sy, MatrixOrderPrepend);
}

void vayu_gui_clip_rect(int64_t h, int64_t x, int64_t y, int64_t w, int64_t k) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GdipSetClipRect(c->g, (REAL)x, (REAL)y, (REAL)w, (REAL)k,
                    CombineModeIntersect);
}

void vayu_gui_clip_reset(int64_t h) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    GdipResetClip(c->g);
}

void vayu_gui_fill_mode(int64_t h, int64_t mode) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c) return;
    c->fill_mode = (mode == 1) ? 1 : 0;
}

/* GDI+ exposes only source-over and source-copy.  XOR would need a blend
   hack; not exposed.  mode: 0=source-over, 1=source-copy. */
void vayu_gui_compositing_mode(int64_t h, int64_t mode) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    c->composite = (mode == 1) ? 1 : 0;
    GdipSetCompositingMode(c->g,
        c->composite ? CompositingModeSourceCopy : CompositingModeSourceOver);
}

/* ===========================================================================
 * Phase 20.5 - Windows Imaging Component (WIC) codec path.
 * Reliable decode/encode on every MinGW-w64 build, unlike GDI+'s codec
 * dispatch which silently returns failure on some stripped libgdiplus.a.
 * ========================================================================= */

static IWICImagingFactory* g_wic_factory = NULL;

static int vayu_wic_ensure(void) {
    if (g_wic_factory) return 1;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* RPC_E_CHANGED_MODE = 0x80010106 - already init'd; still usable. */
    if (FAILED(hr) && hr != (HRESULT)0x80010106L) return 0;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void**)&g_wic_factory);
    if (FAILED(hr) || !g_wic_factory) return 0;
    return 1;
}

static GpBitmap* vayu_wic_load_bitmap(const char* utf8, int len) {
    if (!vayu_wic_ensure()) return NULL;
    WCHAR* wpath = vayu_utf8_to_w(utf8, len);

    IWICBitmapDecoder* dec = NULL;
    HRESULT hr = g_wic_factory->lpVtbl->CreateDecoderFromFilename(
        g_wic_factory, wpath, NULL, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &dec);
    free(wpath);
    if (FAILED(hr) || !dec) return NULL;

    IWICBitmapFrameDecode* frame = NULL;
    hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
    if (FAILED(hr) || !frame) { dec->lpVtbl->Release(dec); return NULL; }

    IWICFormatConverter* conv = NULL;
    hr = g_wic_factory->lpVtbl->CreateFormatConverter(g_wic_factory, &conv);
    if (FAILED(hr) || !conv) {
        frame->lpVtbl->Release(frame); dec->lpVtbl->Release(dec);
        return NULL;
    }

    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource*)frame,
        &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
        NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        conv->lpVtbl->Release(conv); frame->lpVtbl->Release(frame);
        dec->lpVtbl->Release(dec); return NULL;
    }

    UINT ww = 0, hh = 0;
    conv->lpVtbl->GetSize(conv, &ww, &hh);
    if (ww == 0 || hh == 0) {
        conv->lpVtbl->Release(conv); frame->lpVtbl->Release(frame);
        dec->lpVtbl->Release(dec); return NULL;
    }

    UINT stride = ww * 4;
    UINT bufsz  = stride * hh;
    BYTE* pixels = (BYTE*)malloc(bufsz);
    hr = conv->lpVtbl->CopyPixels(conv, NULL, stride, bufsz, pixels);
    conv->lpVtbl->Release(conv);
    frame->lpVtbl->Release(frame);
    dec->lpVtbl->Release(dec);
    if (FAILED(hr)) { free(pixels); return NULL; }

    /* 32bppBGRA in WIC == 32bppARGB as GDI+ names it (both are B,G,R,A in
       memory on little-endian).  Direct handoff works. */
    GpBitmap* bmp = NULL;
    GdipCreateBitmapFromScan0((INT)ww, (INT)hh, (INT)stride,
                              PixelFormat32bppARGB, pixels, &bmp);
    free(pixels);
    return bmp;
}

static int vayu_wic_save_bitmap_png(GpBitmap* bmp, int w, int h,
                                    const char* utf8, int len) {
    if (!bmp) return 0;
    if (!vayu_wic_ensure()) return 0;

    GpRect rc;
    rc.X = 0; rc.Y = 0; rc.Width = w; rc.Height = h;
    BitmapData bd;
    memset(&bd, 0, sizeof(bd));
    if (GdipBitmapLockBits(bmp, &rc, ImageLockModeRead,
                           PixelFormat32bppARGB, &bd) != 0) return 0;

    WCHAR* wpath = vayu_utf8_to_w(utf8, len);

    IWICStream* stream = NULL;
    HRESULT hr = g_wic_factory->lpVtbl->CreateStream(g_wic_factory, &stream);
    if (FAILED(hr) || !stream) {
        GdipBitmapUnlockBits(bmp, &bd); free(wpath); return 0;
    }
    hr = stream->lpVtbl->InitializeFromFilename(stream, wpath, GENERIC_WRITE);
    free(wpath);
    if (FAILED(hr)) {
        stream->lpVtbl->Release(stream);
        GdipBitmapUnlockBits(bmp, &bd); return 0;
    }

    IWICBitmapEncoder* enc = NULL;
    hr = g_wic_factory->lpVtbl->CreateEncoder(g_wic_factory,
                                               &GUID_ContainerFormatPng,
                                               NULL, &enc);
    if (FAILED(hr) || !enc) {
        stream->lpVtbl->Release(stream);
        GdipBitmapUnlockBits(bmp, &bd); return 0;
    }
    hr = enc->lpVtbl->Initialize(enc, (IStream*)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        enc->lpVtbl->Release(enc); stream->lpVtbl->Release(stream);
        GdipBitmapUnlockBits(bmp, &bd); return 0;
    }

    IWICBitmapFrameEncode* frame = NULL;
    IPropertyBag2* props = NULL;
    hr = enc->lpVtbl->CreateNewFrame(enc, &frame, &props);
    if (FAILED(hr) || !frame) {
        enc->lpVtbl->Release(enc); stream->lpVtbl->Release(stream);
        GdipBitmapUnlockBits(bmp, &bd); return 0;
    }
    frame->lpVtbl->Initialize(frame, props);
    if (props) props->lpVtbl->Release(props);
    frame->lpVtbl->SetSize(frame, (UINT)w, (UINT)h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->lpVtbl->SetPixelFormat(frame, &fmt);

    UINT dstStride = (UINT)(w * 4);
    UINT bufsz = dstStride * (UINT)h;
    BYTE* tmp = (BYTE*)malloc(bufsz);
    memset(tmp, 0, bufsz);
    int absStride = bd.Stride < 0 ? -bd.Stride : bd.Stride;
    for (int y = 0; y < h; ++y) {
        BYTE* src = (BYTE*)bd.Scan0 + (size_t)y * (size_t)absStride;
        BYTE* dst = tmp + (size_t)y * (size_t)dstStride;
        memcpy(dst, src, (size_t)w * 4);
    }
    hr = frame->lpVtbl->WritePixels(frame, (UINT)h, dstStride, bufsz, tmp);
    free(tmp);

    frame->lpVtbl->Commit(frame);
    frame->lpVtbl->Release(frame);
    enc->lpVtbl->Commit(enc);
    enc->lpVtbl->Release(enc);
    stream->lpVtbl->Release(stream);

    GdipBitmapUnlockBits(bmp, &bd);
    return SUCCEEDED(hr) ? 1 : 0;
}

/* ===========================================================================
 * Phase 21.1 - software framebuffer + triangle / line raster.
 * Framebuffer is a raw uint32 ARGB buffer owned by C.  present() builds a
 * transient GDI+ bitmap referencing that buffer and blits it to a canvas.
 * ========================================================================= */

typedef struct VayuFramebuffer {
    uint32_t* pixels;
    float*    depth;   /* Phase 21.2 - NULL unless depth enabled */
    int       w, h;
} VayuFramebuffer;

void vayu_raster_fb_clear_depth(int64_t fh);

int64_t vayu_raster_fb_new(int64_t w, int64_t h) {
    if (w <= 0 || h <= 0) return 0;
    VayuFramebuffer* fb = (VayuFramebuffer*)malloc(sizeof(VayuFramebuffer));
    fb->w = (int)w;
    fb->h = (int)h;
    fb->pixels = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(w * h));
    fb->depth  = NULL;
    if (!fb->pixels) { free(fb); return 0; }
    memset(fb->pixels, 0, sizeof(uint32_t) * (size_t)(w * h));
    return (int64_t)fb;
}

void vayu_raster_fb_free(int64_t fh) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb) return;
    if (fb->pixels) free(fb->pixels);
    if (fb->depth)  free(fb->depth);
    free(fb);
}

void vayu_raster_fb_enable_depth(int64_t fh) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb || fb->depth) return;
    fb->depth = (float*)malloc(sizeof(float) * (size_t)(fb->w * fb->h));
    if (fb->depth) vayu_raster_fb_clear_depth(fh);
}

void vayu_raster_fb_disable_depth(int64_t fh) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb || !fb->depth) return;
    free(fb->depth); fb->depth = NULL;
}

void vayu_raster_fb_clear_depth(int64_t fh) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb || !fb->depth) return;
    int64_t n = (int64_t)fb->w * (int64_t)fb->h;
    for (int64_t i = 0; i < n; ++i) fb->depth[i] = 1e30f;
}

int64_t vayu_raster_fb_width(int64_t fh) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    return fb ? (int64_t)fb->w : 0;
}

int64_t vayu_raster_fb_height(int64_t fh) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    return fb ? (int64_t)fb->h : 0;
}

void vayu_raster_fb_clear(int64_t fh, int64_t argb) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb) return;
    uint32_t c = (uint32_t)argb;
    int64_t n = (int64_t)fb->w * (int64_t)fb->h;
    int64_t i = 0;
    while (i < n) { fb->pixels[i] = c; i = i + 1; }
}

void vayu_raster_fb_set(int64_t fh, int64_t x, int64_t y, int64_t argb) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb) return;
    if (x < 0 || x >= fb->w || y < 0 || y >= fb->h) return;
    fb->pixels[(int)y * fb->w + (int)x] = (uint32_t)argb;
}

int64_t vayu_raster_fb_get(int64_t fh, int64_t x, int64_t y) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb) return 0;
    if (x < 0 || x >= fb->w || y < 0 || y >= fb->h) return 0;
    return (int64_t)fb->pixels[(int)y * fb->w + (int)x];
}

void vayu_raster_fb_present(int64_t fh, int64_t canvas_h,
                            int64_t x, int64_t y) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    VayuCanvas* c = (VayuCanvas*)canvas_h;
    if (!fb || !c || !c->g) return;
    GpBitmap* bmp = NULL;
    /* GdipCreateBitmapFromScan0 with a non-NULL scan0 references our
       pixels - no copy.  We dispose the wrapper immediately after the
       draw, so the reference is short-lived and safe. */
    GdipCreateBitmapFromScan0((INT)fb->w, (INT)fb->h, (INT)(fb->w * 4),
                              PixelFormat32bppARGB,
                              (BYTE*)fb->pixels, &bmp);
    if (!bmp) return;
    GdipDrawImageI(c->g, (GpImage*)bmp, (INT)x, (INT)y);
    GdipDisposeImage((GpImage*)bmp);
}

void vayu_raster_draw_line(int64_t fh,
                           int64_t x0, int64_t y0,
                           int64_t x1, int64_t y1,
                           int64_t argb) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb) return;
    int dx = (int)(x1 - x0);
    int dy = (int)(y1 - y0);
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = adx - ady;
    int cx = (int)x0;
    int cy = (int)y0;
    uint32_t col = (uint32_t)argb;
    for (;;) {
        if (cx >= 0 && cx < fb->w && cy >= 0 && cy < fb->h)
            fb->pixels[cy * fb->w + cx] = col;
        if (cx == (int)x1 && cy == (int)y1) break;
        int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; cx += sx; }
        if (e2 <  adx) { err += adx; cy += sy; }
    }
}

void vayu_raster_draw_tri(int64_t fh,
                          int64_t x0, int64_t y0,
                          int64_t x1, int64_t y1,
                          int64_t x2, int64_t y2,
                          int64_t argb) {
    /* unchanged body ... keep the existing implementation verbatim */
    VayuFramebuffer* fb = (VayuFramebuffer*)fh;
    if (!fb) return;

    int minx = (int)x0, maxx = (int)x0;
    int miny = (int)y0, maxy = (int)y0;
    if ((int)x1 < minx) minx = (int)x1;
    if ((int)x1 > maxx) maxx = (int)x1;
    if ((int)x2 < minx) minx = (int)x2;
    if ((int)x2 > maxx) maxx = (int)x2;
    if ((int)y1 < miny) miny = (int)y1;
    if ((int)y1 > maxy) maxy = (int)y1;
    if ((int)y2 < miny) miny = (int)y2;
    if ((int)y2 > maxy) maxy = (int)y2;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= fb->w) maxx = fb->w - 1;
    if (maxy >= fb->h) maxy = fb->h - 1;
    if (minx > maxx || miny > maxy) return;

    int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;

    uint32_t col = (uint32_t)argb;
    int y = miny;
    while (y <= maxy) {
        int x = minx;
        while (x <= maxx) {
            int64_t w0 = (x1 - x) * (y2 - y) - (x2 - x) * (y1 - y);
            int64_t w1 = (x2 - x) * (y0 - y) - (x0 - x) * (y2 - y);
            int64_t w2 = (x0 - x) * (y1 - y) - (x1 - x) * (y0 - y);
            int inside;
            if (area > 0)
                inside = (w0 >= 0 && w1 >= 0 && w2 >= 0);
            else
                inside = (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) fb->pixels[y * fb->w + x] = col;
            x = x + 1;
        }
        y = y + 1;
    }
}

/* ===========================================================================
 * Phase 21.2 + 21.3 - 3D pipeline.
 *   - Matrix stack (Q16.16 cells; double precision internally)
 *   - Indexed mesh
 *   - Texture with mip levels
 *   - Depth-tested, perspective-correct, bilinear / trilinear raster
 *
 * Q16.16 note: all Vayu-side coordinates are fixed-point, 65536 == 1.0.
 * The ABI carries them as int64.  Internally we convert to double for the
 * pipeline math (per-vertex) and use incremental per-pixel scans.
 * ========================================================================= */

typedef struct { int64_t c[16]; } VayuMat4;

typedef struct {
    int64_t* verts;   /* 5 per vertex: x, y, z, u, v  (Q16.16) */
    int64_t* norms;   /* 3 per vertex: nx, ny, nz     (Q16.16) */
    int      nv, cv;
    int32_t* tris;    /* 3 per tri: i0, i1, i2 */
    int      nt, ct;
} VayuMesh;

typedef struct {
    int       w, h;
    uint32_t* pixels;
} VayuTexLevel;

typedef struct {
    VayuTexLevel levels[16];
    int          level_count;
    int          filter;   /* 0=nearest, 1=bilinear, 2=trilinear */
} VayuTexture;

/* ---- matrix ---- */

int64_t vayu_raster_mat_new(void) {
    VayuMat4* m = (VayuMat4*)malloc(sizeof(VayuMat4));
    memset(m->c, 0, sizeof(m->c));
    m->c[0] = m->c[5] = m->c[10] = m->c[15] = 65536;
    return (int64_t)m;
}
void vayu_raster_mat_free(int64_t h) { if (h) free((VayuMat4*)h); }

void vayu_raster_mat_identity(int64_t h) {
    VayuMat4* m = (VayuMat4*)h;
    if (!m) return;
    memset(m->c, 0, sizeof(m->c));
    m->c[0] = m->c[5] = m->c[10] = m->c[15] = 65536;
}

void vayu_raster_mat_mul(int64_t dst_h, int64_t a_h, int64_t b_h) {
    VayuMat4* d = (VayuMat4*)dst_h;
    VayuMat4* a = (VayuMat4*)a_h;
    VayuMat4* b = (VayuMat4*)b_h;
    if (!d || !a || !b) return;
    VayuMat4 t;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                double av = (double)a->c[i*4+k] / 65536.0;
                double bv = (double)b->c[k*4+j] / 65536.0;
                s += av * bv;
            }
            t.c[i*4+j] = (int64_t)(s * 65536.0);
        }
    }
    *d = t;
}

void vayu_raster_mat_translate(int64_t h, int64_t x, int64_t y, int64_t z) {
    VayuMat4* m = (VayuMat4*)h; if (!m) return;
    VayuMat4 T; memset(T.c, 0, sizeof(T.c));
    T.c[0] = T.c[5] = T.c[10] = T.c[15] = 65536;
    T.c[3] = x; T.c[7] = y; T.c[11] = z;
    VayuMat4 tmp; VayuMat4* d = &tmp;
    int64_t dh = (int64_t)d;
    vayu_raster_mat_mul(dh, h, (int64_t)&T);
    *m = tmp;
    (void)d;
}

static double vayu_sin_deg(double d) {
    double r = d * 3.14159265358979323846 / 180.0;
    return sin(r);
}
static double vayu_cos_deg(double d) {
    double r = d * 3.14159265358979323846 / 180.0;
    return cos(r);
}

void vayu_raster_mat_rotate_x(int64_t h, int64_t deg) {
    VayuMat4* m = (VayuMat4*)h; if (!m) return;
    double c = vayu_cos_deg((double)deg);
    double s = vayu_sin_deg((double)deg);
    VayuMat4 R; memset(R.c, 0, sizeof(R.c));
    R.c[0] = R.c[15] = 65536;
    R.c[5]  = (int64_t)(c * 65536.0);
    R.c[6]  = (int64_t)(-s * 65536.0);
    R.c[9]  = (int64_t)(s * 65536.0);
    R.c[10] = (int64_t)(c * 65536.0);
    VayuMat4 tmp;
    vayu_raster_mat_mul((int64_t)&tmp, h, (int64_t)&R);
    *m = tmp;
}

void vayu_raster_mat_rotate_y(int64_t h, int64_t deg) {
    VayuMat4* m = (VayuMat4*)h; if (!m) return;
    double c = vayu_cos_deg((double)deg);
    double s = vayu_sin_deg((double)deg);
    VayuMat4 R; memset(R.c, 0, sizeof(R.c));
    R.c[5] = R.c[15] = 65536;
    R.c[0]  = (int64_t)(c * 65536.0);
    R.c[2]  = (int64_t)(s * 65536.0);
    R.c[8]  = (int64_t)(-s * 65536.0);
    R.c[10] = (int64_t)(c * 65536.0);
    VayuMat4 tmp;
    vayu_raster_mat_mul((int64_t)&tmp, h, (int64_t)&R);
    *m = tmp;
}

void vayu_raster_mat_rotate_z(int64_t h, int64_t deg) {
    VayuMat4* m = (VayuMat4*)h; if (!m) return;
    double c = vayu_cos_deg((double)deg);
    double s = vayu_sin_deg((double)deg);
    VayuMat4 R; memset(R.c, 0, sizeof(R.c));
    R.c[10] = R.c[15] = 65536;
    R.c[0] = (int64_t)(c * 65536.0);
    R.c[1] = (int64_t)(-s * 65536.0);
    R.c[4] = (int64_t)(s * 65536.0);
    R.c[5] = (int64_t)(c * 65536.0);
    VayuMat4 tmp;
    vayu_raster_mat_mul((int64_t)&tmp, h, (int64_t)&R);
    *m = tmp;
}

/* aspect_pct: 100 == 1.0.  Near / far in Q16.16.  Parameter names use
   n_ / f_ because windef.h #defines `near` and `far`. */
void vayu_raster_mat_perspective(int64_t h, int64_t fov_deg,
                                 int64_t aspect_pct,
                                 int64_t n_, int64_t f_) {
    VayuMat4* m = (VayuMat4*)h; if (!m) return;
    double fov = (double)fov_deg * 3.14159265358979323846 / 180.0;
    double f   = 1.0 / tan(fov * 0.5);
    double a   = (double)aspect_pct / 100.0;
    double n   = (double)n_ / 65536.0;
    double ff  = (double)f_ / 65536.0;
    VayuMat4 P; memset(P.c, 0, sizeof(P.c));
    P.c[0]  = (int64_t)((f / a) * 65536.0);
    P.c[5]  = (int64_t)(f * 65536.0);
    P.c[10] = (int64_t)(((ff + n) / (n - ff)) * 65536.0);
    P.c[11] = (int64_t)((2.0 * ff * n / (n - ff)) * 65536.0);
    P.c[14] = (int64_t)(-1.0 * 65536.0);
    P.c[15] = 0;
    *m = P;
}

void vayu_raster_mat_look_at(int64_t h,
                             int64_t ex, int64_t ey, int64_t ez,
                             int64_t tx, int64_t ty, int64_t tz,
                             int64_t ux, int64_t uy, int64_t uz) {
    VayuMat4* m = (VayuMat4*)h; if (!m) return;
    double Ex = (double)ex/65536.0, Ey = (double)ey/65536.0, Ez = (double)ez/65536.0;
    double Tx = (double)tx/65536.0, Ty = (double)ty/65536.0, Tz = (double)tz/65536.0;
    double Ux = (double)ux/65536.0, Uy = (double)uy/65536.0, Uz = (double)uz/65536.0;
    double fx = Tx-Ex, fy = Ty-Ey, fz = Tz-Ez;
    double fl = sqrt(fx*fx+fy*fy+fz*fz);
    if (fl < 1e-9) return;
    fx /= fl; fy /= fl; fz /= fl;
    double sx = fy*Uz - fz*Uy, sy = fz*Ux - fx*Uz, sz = fx*Uy - fy*Ux;
    double sl = sqrt(sx*sx+sy*sy+sz*sz);
    if (sl < 1e-9) return;
    sx /= sl; sy /= sl; sz /= sl;
    double ux2 = sy*fz - sz*fy, uy2 = sz*fx - sx*fz, uz2 = sx*fy - sy*fx;
    VayuMat4 V; memset(V.c, 0, sizeof(V.c));
    V.c[0]  = (int64_t)(sx * 65536.0);
    V.c[1]  = (int64_t)(sy * 65536.0);
    V.c[2]  = (int64_t)(sz * 65536.0);
    V.c[3]  = (int64_t)(-(sx*Ex+sy*Ey+sz*Ez) * 65536.0);
    V.c[4]  = (int64_t)(ux2 * 65536.0);
    V.c[5]  = (int64_t)(uy2 * 65536.0);
    V.c[6]  = (int64_t)(uz2 * 65536.0);
    V.c[7]  = (int64_t)(-(ux2*Ex+uy2*Ey+uz2*Ez) * 65536.0);
    V.c[8]  = (int64_t)(-fx * 65536.0);
    V.c[9]  = (int64_t)(-fy * 65536.0);
    V.c[10] = (int64_t)(-fz * 65536.0);
    V.c[11] = (int64_t)( (fx*Ex+fy*Ey+fz*Ez) * 65536.0);
    V.c[15] = 65536;
    *m = V;
}

/* ---- mesh ---- */

int64_t vayu_raster_mesh_new(void) {
    VayuMesh* m = (VayuMesh*)malloc(sizeof(VayuMesh));
    m->verts = NULL; m->norms = NULL; m->nv = 0; m->cv = 0;
    m->tris  = NULL; m->nt = 0; m->ct = 0;
    return (int64_t)m;
}

void vayu_raster_mesh_free(int64_t h) {
    VayuMesh* m = (VayuMesh*)h;
    if (!m) return;
    if (m->verts) free(m->verts);
    if (m->norms) free(m->norms);
    if (m->tris)  free(m->tris);
    free(m);
}

void vayu_raster_mesh_clear(int64_t h) {
    VayuMesh* m = (VayuMesh*)h;
    if (!m) return;
    m->nv = 0; m->nt = 0;
}

static void vayu_mesh_reserve_v(VayuMesh* m, int need) {
    if (m->nv + need <= m->cv) return;
    int nc = m->cv == 0 ? 32 : m->cv * 2;
    while (nc < m->nv + need) nc *= 2;
    m->verts = (int64_t*)realloc(m->verts, sizeof(int64_t) * 5 * (size_t)nc);
    m->norms = (int64_t*)realloc(m->norms, sizeof(int64_t) * 3 * (size_t)nc);
    m->cv = nc;
}
static void vayu_mesh_reserve_t(VayuMesh* m, int need) {
    if (m->nt + need <= m->ct) return;
    int nc = m->ct == 0 ? 32 : m->ct * 2;
    while (nc < m->nt + need) nc *= 2;
    m->tris = (int32_t*)realloc(m->tris, sizeof(int32_t) * 3 * (size_t)nc);
    m->ct = nc;
}

void vayu_raster_mesh_add_vert(int64_t h, int64_t x, int64_t y, int64_t z,
                               int64_t u, int64_t v) {
    VayuMesh* m = (VayuMesh*)h;
    if (!m) return;
    vayu_mesh_reserve_v(m, 1);
    int64_t* p = m->verts + 5 * (size_t)m->nv;
    p[0] = x; p[1] = y; p[2] = z; p[3] = u; p[4] = v;
    int64_t* nn = m->norms + 3 * (size_t)m->nv;
    nn[0] = 0; nn[1] = 0; nn[2] = 65536;   /* default +Z */
    m->nv++;
}

void vayu_raster_mesh_add_vert_lit(int64_t h,
                                   int64_t x, int64_t y, int64_t z,
                                   int64_t nx, int64_t ny, int64_t nz,
                                   int64_t u, int64_t v) {
    VayuMesh* m = (VayuMesh*)h;
    if (!m) return;
    vayu_mesh_reserve_v(m, 1);
    int64_t* p = m->verts + 5 * (size_t)m->nv;
    p[0] = x; p[1] = y; p[2] = z; p[3] = u; p[4] = v;
    int64_t* nn = m->norms + 3 * (size_t)m->nv;
    nn[0] = nx; nn[1] = ny; nn[2] = nz;
    m->nv++;
}

void vayu_raster_mesh_set_normal(int64_t h, int64_t idx,
                                 int64_t nx, int64_t ny, int64_t nz) {
    VayuMesh* m = (VayuMesh*)h;
    if (!m) return;
    if (idx < 0 || idx >= m->nv) return;
    int64_t* nn = m->norms + 3 * (size_t)idx;
    nn[0] = nx; nn[1] = ny; nn[2] = nz;
}

void vayu_raster_mesh_add_tri(int64_t h, int64_t i0, int64_t i1, int64_t i2) {
    VayuMesh* m = (VayuMesh*)h;
    if (!m) return;
    vayu_mesh_reserve_t(m, 1);
    int32_t* p = m->tris + 3 * (size_t)m->nt;
    p[0] = (int32_t)i0; p[1] = (int32_t)i1; p[2] = (int32_t)i2;
    m->nt++;
}

/* ---- texture ---- */

int64_t vayu_raster_tex_new(int64_t w, int64_t h) {
    if (w <= 0 || h <= 0) return 0;
    VayuTexture* t = (VayuTexture*)malloc(sizeof(VayuTexture));
    memset(t, 0, sizeof(*t));
    t->filter = 1;
    t->levels[0].w = (int)w;
    t->levels[0].h = (int)h;
    t->levels[0].pixels = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(w*h));
    if (!t->levels[0].pixels) { free(t); return 0; }
    memset(t->levels[0].pixels, 0, sizeof(uint32_t) * (size_t)(w*h));
    t->level_count = 1;
    return (int64_t)t;
}

void vayu_raster_tex_free(int64_t h) {
    VayuTexture* t = (VayuTexture*)h;
    if (!t) return;
    for (int i = 0; i < t->level_count; ++i)
        if (t->levels[i].pixels) free(t->levels[i].pixels);
    free(t);
}

int64_t vayu_raster_tex_width(int64_t h)  { VayuTexture* t=(VayuTexture*)h; return t?t->levels[0].w:0; }
int64_t vayu_raster_tex_height(int64_t h) { VayuTexture* t=(VayuTexture*)h; return t?t->levels[0].h:0; }

void vayu_raster_tex_set(int64_t h, int64_t x, int64_t y, int64_t argb) {
    VayuTexture* t = (VayuTexture*)h;
    if (!t) return;
    if (x < 0 || x >= t->levels[0].w || y < 0 || y >= t->levels[0].h) return;
    t->levels[0].pixels[(int)y * t->levels[0].w + (int)x] = (uint32_t)argb;
}

void vayu_raster_tex_set_filter(int64_t h, int64_t mode) {
    VayuTexture* t = (VayuTexture*)h;
    if (!t) return;
    t->filter = (mode >= 0 && mode <= 2) ? (int)mode : 1;
}

void vayu_raster_tex_gen_mipmaps(int64_t h) {
    VayuTexture* t = (VayuTexture*)h;
    if (!t) return;
    int cur_w = t->levels[0].w, cur_h = t->levels[0].h;
    int src = 0;
    while ((cur_w > 1 || cur_h > 1) && t->level_count < 16) {
        int nw = cur_w > 1 ? cur_w / 2 : 1;
        int nh = cur_h > 1 ? cur_h / 2 : 1;
        int dst = t->level_count;
        t->levels[dst].w = nw;
        t->levels[dst].h = nh;
        t->levels[dst].pixels = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(nw*nh));
        if (!t->levels[dst].pixels) break;
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                uint32_t sum_b = 0, sum_g = 0, sum_r = 0, sum_a = 0;
                int n = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = x*2 + dx;
                        int sy = y*2 + dy;
                        if (sx >= cur_w) sx = cur_w - 1;
                        if (sy >= cur_h) sy = cur_h - 1;
                        uint32_t c = t->levels[src].pixels[sy*cur_w + sx];
                        sum_b +=  c        & 0xFF;
                        sum_g += (c >>  8) & 0xFF;
                        sum_r += (c >> 16) & 0xFF;
                        sum_a += (c >> 24) & 0xFF;
                        n++;
                    }
                }
                uint32_t avg = ((sum_a/n) << 24) | ((sum_r/n) << 16)
                             | ((sum_g/n) <<  8) |  (sum_b/n);
                t->levels[dst].pixels[y*nw + x] = avg;
            }
        }
        t->level_count = dst + 1;
        cur_w = nw; cur_h = nh; src = dst;
    }
}

/* nearest sample at explicit level */
static uint32_t vayu_tex_nearest(const VayuTexLevel* L, double u, double v) {
    int x = (int)(u * L->w);
    int y = (int)(v * L->h);
    if (x < 0) x = 0; if (x >= L->w) x = L->w - 1;
    if (y < 0) y = 0; if (y >= L->h) y = L->h - 1;
    return L->pixels[y * L->w + x];
}

/* bilinear sample at explicit level */
static uint32_t vayu_tex_bilinear(const VayuTexLevel* L, double u, double v) {
    double fx = u * L->w - 0.5;
    double fy = v * L->h - 0.5;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    double dx = fx - x0, dy = fy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) x0 = 0; if (x0 >= L->w) x0 = L->w - 1;
    if (y0 < 0) y0 = 0; if (y0 >= L->h) y0 = L->h - 1;
    if (x1 < 0) x1 = 0; if (x1 >= L->w) x1 = L->w - 1;
    if (y1 < 0) y1 = 0; if (y1 >= L->h) y1 = L->h - 1;
    uint32_t c00 = L->pixels[y0*L->w + x0];
    uint32_t c10 = L->pixels[y0*L->w + x1];
    uint32_t c01 = L->pixels[y1*L->w + x0];
    uint32_t c11 = L->pixels[y1*L->w + x1];
    double w00 = (1-dx)*(1-dy), w10 = dx*(1-dy);
    double w01 = (1-dx)*dy,     w11 = dx*dy;
    uint32_t a = (uint32_t)(((c00>>24)&0xFF)*w00 + ((c10>>24)&0xFF)*w10
                           +((c01>>24)&0xFF)*w01 + ((c11>>24)&0xFF)*w11 + 0.5);
    uint32_t r = (uint32_t)(((c00>>16)&0xFF)*w00 + ((c10>>16)&0xFF)*w10
                           +((c01>>16)&0xFF)*w01 + ((c11>>16)&0xFF)*w11 + 0.5);
    uint32_t g = (uint32_t)(((c00>> 8)&0xFF)*w00 + ((c10>> 8)&0xFF)*w10
                           +((c01>> 8)&0xFF)*w01 + ((c11>> 8)&0xFF)*w11 + 0.5);
    uint32_t b = (uint32_t)(((c00     )&0xFF)*w00 + ((c10     )&0xFF)*w10
                           +((c01     )&0xFF)*w01 + ((c11     )&0xFF)*w11 + 0.5);
    return (a<<24) | (r<<16) | (g<<8) | b;
}

static uint32_t vayu_tex_sample(VayuTexture* t, double u, double v, double lod_f) {
    int lc = t->level_count;
    if (lc == 0) return 0xFFFF00FFu;
    if (t->filter == 0) {
        int lvl = (int)(lod_f + 0.5);
        if (lvl < 0) lvl = 0; if (lvl >= lc) lvl = lc - 1;
        return vayu_tex_nearest(&t->levels[lvl], u, v);
    }
    if (t->filter == 1 || lc == 1) {
        int lvl = (int)(lod_f + 0.5);
        if (lvl < 0) lvl = 0; if (lvl >= lc) lvl = lc - 1;
        return vayu_tex_bilinear(&t->levels[lvl], u, v);
    }
    /* trilinear */
    double lf = lod_f;
    if (lf < 0) lf = 0;
    int l0 = (int)floor(lf);
    if (l0 >= lc - 1) return vayu_tex_bilinear(&t->levels[lc-1], u, v);
    int l1 = l0 + 1;
    double f = lf - l0;
    uint32_t a = vayu_tex_bilinear(&t->levels[l0], u, v);
    uint32_t b = vayu_tex_bilinear(&t->levels[l1], u, v);
    double fb = f, fa = 1.0 - f;
    uint32_t oa = (uint32_t)((((a>>24)&0xFF)*fa) + (((b>>24)&0xFF)*fb) + 0.5);
    uint32_t or_ = (uint32_t)((((a>>16)&0xFF)*fa) + (((b>>16)&0xFF)*fb) + 0.5);
    uint32_t og = (uint32_t)((((a>> 8)&0xFF)*fa) + (((b>> 8)&0xFF)*fb) + 0.5);
    uint32_t ob = (uint32_t)((((a     )&0xFF)*fa) + (((b     )&0xFF)*fb) + 0.5);
    return (oa<<24) | (or_<<16) | (og<<8) | ob;
}

/* ---- draw_mesh ----
 * All inputs are Q16.16 unless noted.  tint_argb is 0xAARRGGBB; pass
 * 0xFFFFFFFF for no tint.
 */
typedef struct { double x, y, z, w, u, v; } Vtx;

static Vtx vayu_transform_vtx(const double M[16], const int64_t* vp) {
    double x = (double)vp[0] / 65536.0;
    double y = (double)vp[1] / 65536.0;
    double z = (double)vp[2] / 65536.0;
    double u = (double)vp[3] / 65536.0;
    double vv= (double)vp[4] / 65536.0;
    Vtx r;
    r.x = M[0]*x  + M[1]*y  + M[2]*z  + M[3];
    r.y = M[4]*x  + M[5]*y  + M[6]*z  + M[7];
    r.z = M[8]*x  + M[9]*y  + M[10]*z + M[11];
    r.w = M[12]*x + M[13]*y + M[14]*z + M[15];
    r.u = u; r.v = vv;
    return r;
}

static double vayu_edge_fn(double ax, double ay, double bx, double by,
                           double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void vayu_raster_draw_mesh(int64_t fbh, int64_t meshh, int64_t math,
                           int64_t texh, int64_t tint) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fbh;
    VayuMesh* mesh = (VayuMesh*)meshh;
    VayuMat4* M = (VayuMat4*)math;
    VayuTexture* tex = (VayuTexture*)texh;
    if (!fb || !mesh || !M) return;

    double md[16];
    for (int i = 0; i < 16; ++i) md[i] = (double)M->c[i] / 65536.0;

    uint32_t tint_a = (uint32_t)((tint >> 24) & 0xFF);
    uint32_t tint_r = (uint32_t)((tint >> 16) & 0xFF);
    uint32_t tint_g = (uint32_t)((tint >>  8) & 0xFF);
    uint32_t tint_b = (uint32_t)((tint      ) & 0xFF);
    int apply_tint = (tint != (int64_t)0xFFFFFFFF);

    for (int ti = 0; ti < mesh->nt; ++ti) {
        int i0 = mesh->tris[ti*3];
        int i1 = mesh->tris[ti*3+1];
        int i2 = mesh->tris[ti*3+2];
        if (i0 < 0 || i0 >= mesh->nv) continue;
        if (i1 < 0 || i1 >= mesh->nv) continue;
        if (i2 < 0 || i2 >= mesh->nv) continue;

        Vtx a = vayu_transform_vtx(md, mesh->verts + 5*(size_t)i0);
        Vtx b = vayu_transform_vtx(md, mesh->verts + 5*(size_t)i1);
        Vtx c = vayu_transform_vtx(md, mesh->verts + 5*(size_t)i2);
        if (a.w <= 1e-9 || b.w <= 1e-9 || c.w <= 1e-9) continue;

        /* NDC + viewport.  Y flip for screen coords (top-left origin). */
        double ax = ((a.x/a.w) + 1.0) * 0.5 * (double)fb->w;
        double ay = (1.0 - (a.y/a.w)) * 0.5 * (double)fb->h;
        double bx = ((b.x/b.w) + 1.0) * 0.5 * (double)fb->w;
        double by = (1.0 - (b.y/b.w)) * 0.5 * (double)fb->h;
        double cx = ((c.x/c.w) + 1.0) * 0.5 * (double)fb->w;
        double cy = (1.0 - (c.y/c.w)) * 0.5 * (double)fb->h;

        double az = a.z / a.w, bz = b.z / b.w, cz = c.z / c.w;

        double aIW = 1.0 / a.w, bIW = 1.0 / b.w, cIW = 1.0 / c.w;
        double aUoW = a.u * aIW, bUoW = b.u * bIW, cUoW = c.u * cIW;
        double aVoW = a.v * aIW, bVoW = b.v * bIW, cVoW = c.v * cIW;

        int minx = (int)floor(ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx));
        int maxx = (int)ceil (ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx));
        int miny = (int)floor(ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy));
        int maxy = (int)ceil (ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx >= fb->w) maxx = fb->w - 1;
        if (maxy >= fb->h) maxy = fb->h - 1;
        if (minx > maxx || miny > maxy) continue;

        double area = vayu_edge_fn(ax,ay, bx,by, cx,cy);
        if (area > -1e-9 && area < 1e-9) continue;
        double inv_area = 1.0 / area;

        /* LOD selection: texel-per-pixel ratio across the triangle. */
        double lod = 0.0;
        if (tex && tex->level_count > 1) {
            double du = fabs(a.u - b.u) + fabs(a.u - c.u);
            double dv = fabs(a.v - b.v) + fabs(a.v - c.v);
            double uv_span = sqrt(du*du + dv*dv);
            double dX = fabs(ax - bx) + fabs(ax - cx);
            double dY = fabs(ay - by) + fabs(ay - cy);
            double s_span = sqrt(dX*dX + dY*dY);
            if (s_span > 1e-9) {
                double texels_per_px = (uv_span / s_span) * tex->levels[0].w;
                if (texels_per_px > 1e-9) lod = log2(texels_per_px);
                if (lod < 0) lod = 0;
            }
        }

        int py = miny;
        while (py <= maxy) {
            double fy = py + 0.5;
            int px = minx;
            while (px <= maxx) {
                double fx = px + 0.5;
                double w0 = vayu_edge_fn(bx,by, cx,cy, fx,fy);
                double w1 = vayu_edge_fn(cx,cy, ax,ay, fx,fy);
                double w2 = vayu_edge_fn(ax,ay, bx,by, fx,fy);
                int inside;
                if (area > 0) inside = (w0>=0 && w1>=0 && w2>=0);
                else          inside = (w0<=0 && w1<=0 && w2<=0);
                if (!inside) { px = px + 1; continue; }

                double l0 = w0 * inv_area;
                double l1 = w1 * inv_area;
                double l2 = w2 * inv_area;

                double z = l0*az + l1*bz + l2*cz;
                int pidx = py * fb->w + px;
                if (fb->depth) {
                    if (z >= (double)fb->depth[pidx]) { px = px + 1; continue; }
                    fb->depth[pidx] = (float)z;
                }

                uint32_t out;
                if (tex) {
                    double iw = l0*aIW + l1*bIW + l2*cIW;
                    if (iw <= 1e-12) { px = px + 1; continue; }
                    double u = (l0*aUoW + l1*bUoW + l2*cUoW) / iw;
                    double v = (l0*aVoW + l1*bVoW + l2*cVoW) / iw;
                    u = u - floor(u);  /* wrap */
                    v = v - floor(v);
                    out = vayu_tex_sample(tex, u, v, lod);
                    if (apply_tint) {
                        uint32_t oa = ((out >> 24) & 0xFF) * tint_a / 255u;
                        uint32_t or_= ((out >> 16) & 0xFF) * tint_r / 255u;
                        uint32_t og = ((out >>  8) & 0xFF) * tint_g / 255u;
                        uint32_t ob = ((out      ) & 0xFF) * tint_b / 255u;
                        out = (oa<<24) | (or_<<16) | (og<<8) | ob;
                    }
                } else {
                    out = (uint32_t)tint;
                }
                fb->pixels[pidx] = out;
                px = px + 1;
            }
            py = py + 1;
        }
    }
}

/* ===========================================================================
 * Phase 21.4 - Camera + lighting (Gouraud, per-vertex).
 * Light direction convention: for kind==0, (x,y,z) points FROM the surface
 * TOWARD the light.  For kind==1, (x,y,z) is the world-space position.
 * ========================================================================= */

#define VAYU_MAX_LIGHTS 8

typedef struct {
    int      kind;
    double   x, y, z;
    uint32_t argb;
    double   intensity;
} VayuLight;

static VayuLight g_lights[VAYU_MAX_LIGHTS];
static int       g_light_n = 0;
static uint32_t  g_ambient = 0xFF202028u;

void vayu_raster_set_ambient(int64_t argb) {
    g_ambient = (uint32_t)argb;
}

void vayu_raster_light_clear(void) { g_light_n = 0; }

void vayu_raster_light_set(int64_t idx, int64_t kind,
                           int64_t x, int64_t y, int64_t z,
                           int64_t argb, int64_t intensity_q16) {
    if (idx < 0 || idx >= VAYU_MAX_LIGHTS) return;
    g_lights[idx].kind = (int)kind;
    g_lights[idx].x = (double)x / 65536.0;
    g_lights[idx].y = (double)y / 65536.0;
    g_lights[idx].z = (double)z / 65536.0;
    g_lights[idx].argb = (uint32_t)argb;
    g_lights[idx].intensity = (double)intensity_q16 / 65536.0;
    if (idx >= g_light_n) g_light_n = (int)idx + 1;
}

static void vayu_light_vertex(double px, double py, double pz,
                              double nx, double ny, double nz,
                              double vx, double vy, double vz,
                              int shade_mode,
                              double out[3]) {
    out[0] = (double)((g_ambient >> 16) & 0xFF) / 255.0;
    out[1] = (double)((g_ambient >>  8) & 0xFF) / 255.0;
    out[2] = (double)((g_ambient      ) & 0xFF) / 255.0;

    double nl = sqrt(nx*nx + ny*ny + nz*nz);
    if (nl > 1e-9) { nx /= nl; ny /= nl; nz /= nl; }
    else           { nx = 0; ny = 0; nz = 1; }

    for (int i = 0; i < g_light_n; ++i) {
        VayuLight* L = &g_lights[i];
        double lx, ly, lz;
        if (L->kind == 0) {
            lx = L->x; ly = L->y; lz = L->z;
            double ll = sqrt(lx*lx + ly*ly + lz*lz);
            if (ll < 1e-9) continue;
            lx /= ll; ly /= ll; lz /= ll;
        } else {
            lx = L->x - px; ly = L->y - py; lz = L->z - pz;
            double ll = sqrt(lx*lx + ly*ly + lz*lz);
            if (ll < 1e-9) continue;
            lx /= ll; ly /= ll; lz /= ll;
        }
        double ndotl = nx*lx + ny*ly + nz*lz;
        if (ndotl < 0.0) ndotl = 0.0;

        double lr = (double)((L->argb >> 16) & 0xFF) / 255.0;
        double lg = (double)((L->argb >>  8) & 0xFF) / 255.0;
        double lb = (double)((L->argb      ) & 0xFF) / 255.0;

        out[0] += ndotl * lr * L->intensity;
        out[1] += ndotl * lg * L->intensity;
        out[2] += ndotl * lb * L->intensity;

        if (shade_mode >= 2 && ndotl > 0.0) {
            double hx = lx + vx, hy = ly + vy, hz = lz + vz;
            double hl = sqrt(hx*hx + hy*hy + hz*hz);
            if (hl > 1e-9) {
                hx /= hl; hy /= hl; hz /= hl;
                double ndoth = nx*hx + ny*hy + nz*hz;
                if (ndoth > 0.0) {
                    double spec = pow(ndoth, 32.0);
                    out[0] += spec * lr * L->intensity;
                    out[1] += spec * lg * L->intensity;
                    out[2] += spec * lb * L->intensity;
                }
            }
        }
    }
}

void vayu_raster_draw_mesh_lit(int64_t fbh, int64_t meshh,
                               int64_t mvp_h, int64_t model_h,
                               int64_t texh, int64_t shade_mode,
                               int64_t tint,
                               int64_t eye_x, int64_t eye_y, int64_t eye_z) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fbh;
    VayuMesh* mesh = (VayuMesh*)meshh;
    VayuMat4* MVP = (VayuMat4*)mvp_h;
    VayuMat4* MOD = (VayuMat4*)model_h;
    VayuTexture* tex = (VayuTexture*)texh;
    if (!fb || !mesh || !MVP || !MOD) return;

    double md[16], mv[16];
    for (int i = 0; i < 16; ++i) {
        md[i] = (double)MOD->c[i] / 65536.0;
        mv[i] = (double)MVP->c[i] / 65536.0;
    }
    double ex = (double)eye_x / 65536.0;
    double ey = (double)eye_y / 65536.0;
    double ez = (double)eye_z / 65536.0;

    uint32_t tint_a = (uint32_t)((tint >> 24) & 0xFF);
    uint32_t tint_r = (uint32_t)((tint >> 16) & 0xFF);
    uint32_t tint_g = (uint32_t)((tint >>  8) & 0xFF);
    uint32_t tint_b = (uint32_t)((tint      ) & 0xFF);
    int apply_tint = (tint != (int64_t)0xFFFFFFFF);

    for (int ti = 0; ti < mesh->nt; ++ti) {
        int i0 = mesh->tris[ti*3];
        int i1 = mesh->tris[ti*3+1];
        int i2 = mesh->tris[ti*3+2];
        if (i0 < 0 || i0 >= mesh->nv) continue;
        if (i1 < 0 || i1 >= mesh->nv) continue;
        if (i2 < 0 || i2 >= mesh->nv) continue;

        const int64_t* v0 = mesh->verts + 5*(size_t)i0;
        const int64_t* v1 = mesh->verts + 5*(size_t)i1;
        const int64_t* v2 = mesh->verts + 5*(size_t)i2;
        const int64_t* n0 = mesh->norms + 3*(size_t)i0;
        const int64_t* n1 = mesh->norms + 3*(size_t)i1;
        const int64_t* n2 = mesh->norms + 3*(size_t)i2;

        double lx0 = (double)v0[0]/65536.0, ly0 = (double)v0[1]/65536.0, lz0 = (double)v0[2]/65536.0;
        double lx1 = (double)v1[0]/65536.0, ly1 = (double)v1[1]/65536.0, lz1 = (double)v1[2]/65536.0;
        double lx2 = (double)v2[0]/65536.0, ly2 = (double)v2[1]/65536.0, lz2 = (double)v2[2]/65536.0;

        double wx0 = md[0]*lx0 + md[1]*ly0 + md[2]*lz0  + md[3];
        double wy0 = md[4]*lx0 + md[5]*ly0 + md[6]*lz0  + md[7];
        double wz0 = md[8]*lx0 + md[9]*ly0 + md[10]*lz0 + md[11];
        double wx1 = md[0]*lx1 + md[1]*ly1 + md[2]*lz1  + md[3];
        double wy1 = md[4]*lx1 + md[5]*ly1 + md[6]*lz1  + md[7];
        double wz1 = md[8]*lx1 + md[9]*ly1 + md[10]*lz1 + md[11];
        double wx2 = md[0]*lx2 + md[1]*ly2 + md[2]*lz2  + md[3];
        double wy2 = md[4]*lx2 + md[5]*ly2 + md[6]*lz2  + md[7];
        double wz2 = md[8]*lx2 + md[9]*ly2 + md[10]*lz2 + md[11];

        double ax = (double)n0[0]/65536.0, ay = (double)n0[1]/65536.0, az = (double)n0[2]/65536.0;
        double bx = (double)n1[0]/65536.0, by = (double)n1[1]/65536.0, bz = (double)n1[2]/65536.0;
        double ccx= (double)n2[0]/65536.0, ccy= (double)n2[1]/65536.0, ccz= (double)n2[2]/65536.0;
        double nx0 = md[0]*ax + md[1]*ay + md[2]*az;
        double ny0 = md[4]*ax + md[5]*ay + md[6]*az;
        double nz0 = md[8]*ax + md[9]*ay + md[10]*az;
        double nx1 = md[0]*bx + md[1]*by + md[2]*bz;
        double ny1 = md[4]*bx + md[5]*by + md[6]*bz;
        double nz1 = md[8]*bx + md[9]*by + md[10]*bz;
        double nx2 = md[0]*ccx + md[1]*ccy + md[2]*ccz;
        double ny2 = md[4]*ccx + md[5]*ccy + md[6]*ccz;
        double nz2 = md[8]*ccx + md[9]*ccy + md[10]*ccz;

        double vx0=ex-wx0, vy0=ey-wy0, vz0=ez-wz0;
        double vl0 = sqrt(vx0*vx0+vy0*vy0+vz0*vz0);
        if (vl0 > 1e-9) { vx0/=vl0; vy0/=vl0; vz0/=vl0; }
        double vx1=ex-wx1, vy1=ey-wy1, vz1=ez-wz1;
        double vl1 = sqrt(vx1*vx1+vy1*vy1+vz1*vz1);
        if (vl1 > 1e-9) { vx1/=vl1; vy1/=vl1; vz1/=vl1; }
        double vx2=ex-wx2, vy2=ey-wy2, vz2=ez-wz2;
        double vl2 = sqrt(vx2*vx2+vy2*vy2+vz2*vz2);
        if (vl2 > 1e-9) { vx2/=vl2; vy2/=vl2; vz2/=vl2; }

        double c0[3], c1[3], c2[3];
        vayu_light_vertex(wx0,wy0,wz0, nx0,ny0,nz0, vx0,vy0,vz0, (int)shade_mode, c0);
        vayu_light_vertex(wx1,wy1,wz1, nx1,ny1,nz1, vx1,vy1,vz1, (int)shade_mode, c1);
        vayu_light_vertex(wx2,wy2,wz2, nx2,ny2,nz2, vx2,vy2,vz2, (int)shade_mode, c2);

        double cx0 = mv[0]*lx0  + mv[1]*ly0  + mv[2]*lz0  + mv[3];
        double cy0 = mv[4]*lx0  + mv[5]*ly0  + mv[6]*lz0  + mv[7];
        double cz0 = mv[8]*lx0  + mv[9]*ly0  + mv[10]*lz0 + mv[11];
        double cw0 = mv[12]*lx0 + mv[13]*ly0 + mv[14]*lz0 + mv[15];
        double cx1 = mv[0]*lx1  + mv[1]*ly1  + mv[2]*lz1  + mv[3];
        double cy1 = mv[4]*lx1  + mv[5]*ly1  + mv[6]*lz1  + mv[7];
        double cz1 = mv[8]*lx1  + mv[9]*ly1  + mv[10]*lz1 + mv[11];
        double cw1 = mv[12]*lx1 + mv[13]*ly1 + mv[14]*lz1 + mv[15];
        double cx2 = mv[0]*lx2  + mv[1]*ly2  + mv[2]*lz2  + mv[3];
        double cy2 = mv[4]*lx2  + mv[5]*ly2  + mv[6]*lz2  + mv[7];
        double cz2 = mv[8]*lx2  + mv[9]*ly2  + mv[10]*lz2 + mv[11];
        double cw2 = mv[12]*lx2 + mv[13]*ly2 + mv[14]*lz2 + mv[15];
        if (cw0 <= 1e-9 || cw1 <= 1e-9 || cw2 <= 1e-9) continue;

        double sxa = ((cx0/cw0)+1.0)*0.5*(double)fb->w;
        double sya = (1.0-(cy0/cw0))*0.5*(double)fb->h;
        double sxb = ((cx1/cw1)+1.0)*0.5*(double)fb->w;
        double syb = (1.0-(cy1/cw1))*0.5*(double)fb->h;
        double sxc = ((cx2/cw2)+1.0)*0.5*(double)fb->w;
        double syc = (1.0-(cy2/cw2))*0.5*(double)fb->h;
        double dza = cz0/cw0, dzb = cz1/cw1, dzc = cz2/cw2;

        double aIW = 1.0/cw0, bIW = 1.0/cw1, cIW = 1.0/cw2;
        double aU = (double)v0[3]/65536.0, bU = (double)v1[3]/65536.0, cU = (double)v2[3]/65536.0;
        double aV = (double)v0[4]/65536.0, bV = (double)v1[4]/65536.0, cV = (double)v2[4]/65536.0;
        double aUoW = aU*aIW, bUoW = bU*bIW, cUoW = cU*cIW;
        double aVoW = aV*aIW, bVoW = bV*bIW, cVoW = cV*cIW;

        int minx = (int)floor(sxa<sxb ? (sxa<sxc?sxa:sxc) : (sxb<sxc?sxb:sxc));
        int maxx = (int)ceil (sxa>sxb ? (sxa>sxc?sxa:sxc) : (sxb>sxc?sxb:sxc));
        int miny = (int)floor(sya<syb ? (sya<syc?sya:syc) : (syb<syc?syb:syc));
        int maxy = (int)ceil (sya>syb ? (sya>syc?sya:syc) : (syb>syc?syb:syc));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx >= fb->w) maxx = fb->w - 1;
        if (maxy >= fb->h) maxy = fb->h - 1;
        if (minx > maxx || miny > maxy) continue;

        double area = vayu_edge_fn(sxa,sya, sxb,syb, sxc,syc);
        if (area > -1e-9 && area < 1e-9) continue;
        double inv_area = 1.0 / area;

        double lod = 0.0;
        if (tex && tex->level_count > 1) {
            double du = fabs(aU - bU) + fabs(aU - cU);
            double dv = fabs(aV - bV) + fabs(aV - cV);
            double uvs = sqrt(du*du + dv*dv);
            double dX = fabs(sxa - sxb) + fabs(sxa - sxc);
            double dY = fabs(sya - syb) + fabs(sya - syc);
            double ss = sqrt(dX*dX + dY*dY);
            if (ss > 1e-9) {
                double tpp = (uvs / ss) * tex->levels[0].w;
                if (tpp > 1e-9) lod = log2(tpp);
                if (lod < 0) lod = 0;
            }
        }

        for (int py = miny; py <= maxy; ++py) {
            double fy = py + 0.5;
            for (int px = minx; px <= maxx; ++px) {
                double fx = px + 0.5;
                double w0 = vayu_edge_fn(sxb,syb, sxc,syc, fx,fy);
                double w1 = vayu_edge_fn(sxc,syc, sxa,sya, fx,fy);
                double w2 = vayu_edge_fn(sxa,sya, sxb,syb, fx,fy);
                int inside;
                if (area > 0) inside = (w0>=0 && w1>=0 && w2>=0);
                else          inside = (w0<=0 && w1<=0 && w2<=0);
                if (!inside) continue;

                double l0 = w0*inv_area, l1 = w1*inv_area, l2 = w2*inv_area;
                double z = l0*dza + l1*dzb + l2*dzc;
                int pidx = py*fb->w + px;
                if (fb->depth) {
                    if (z >= (double)fb->depth[pidx]) continue;
                    fb->depth[pidx] = (float)z;
                }

                double vr = l0*c0[0] + l1*c1[0] + l2*c2[0];
                double vg = l0*c0[1] + l1*c1[1] + l2*c2[1];
                double vb = l0*c0[2] + l1*c1[2] + l2*c2[2];
                if (vr > 1.0) vr = 1.0; if (vr < 0.0) vr = 0.0;
                if (vg > 1.0) vg = 1.0; if (vg < 0.0) vg = 0.0;
                if (vb > 1.0) vb = 1.0; if (vb < 0.0) vb = 0.0;

                uint32_t out;
                if (tex) {
                    double iw = l0*aIW + l1*bIW + l2*cIW;
                    if (iw <= 1e-12) continue;
                    double u = (l0*aUoW + l1*bUoW + l2*cUoW) / iw;
                    double v = (l0*aVoW + l1*bVoW + l2*cVoW) / iw;
                    u = u - floor(u);
                    v = v - floor(v);
                    uint32_t texel = vayu_tex_sample(tex, u, v, lod);
                    uint32_t ta = (texel >> 24) & 0xFF;
                    uint32_t tr = (texel >> 16) & 0xFF;
                    uint32_t tg = (texel >>  8) & 0xFF;
                    uint32_t tb = (texel      ) & 0xFF;
                    out = (ta<<24)
                        | ((uint32_t)(tr * vr) << 16)
                        | ((uint32_t)(tg * vg) <<  8)
                        | ((uint32_t)(tb * vb));
                } else {
                    out = (0xFFu << 24)
                        | ((uint32_t)(vr*255.0+0.5) << 16)
                        | ((uint32_t)(vg*255.0+0.5) <<  8)
                        | ((uint32_t)(vb*255.0+0.5));
                }
                if (apply_tint) {
                    uint32_t oa = ((out >> 24) & 0xFF) * tint_a / 255u;
                    uint32_t or_= ((out >> 16) & 0xFF) * tint_r / 255u;
                    uint32_t og = ((out >>  8) & 0xFF) * tint_g / 255u;
                    uint32_t ob = ((out      ) & 0xFF) * tint_b / 255u;
                    out = (oa<<24) | (or_<<16) | (og<<8) | ob;
                }
                fb->pixels[pidx] = out;
            }
        }
    }
}

/* ===========================================================================
 * Phase 21.5 (revised) - Post-processing.
 * Per-pixel passes over texture level 0, in place.
 * ========================================================================= */

void vayu_raster_post_gamma(int64_t texh, int64_t gamma_q16) {
    VayuTexture* t = (VayuTexture*)texh;
    if (!t || t->level_count < 1) return;
    double g = (double)gamma_q16 / 65536.0;
    if (g <= 0.0) g = 1.0;
    double inv = 1.0 / g;
    VayuTexLevel* L = &t->levels[0];
    int64_t n = (int64_t)L->w * (int64_t)L->h;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t c = L->pixels[i];
        uint32_t a = (c >> 24) & 0xFF;
        double r = pow((double)((c >> 16) & 0xFF) / 255.0, inv);
        double gg= pow((double)((c >>  8) & 0xFF) / 255.0, inv);
        double b = pow((double)( c        & 0xFF) / 255.0, inv);
        L->pixels[i] = (a<<24)
                     | ((uint32_t)(r *255.0+0.5) << 16)
                     | ((uint32_t)(gg*255.0+0.5) <<  8)
                     |  (uint32_t)(b *255.0+0.5);
    }
}

void vayu_raster_post_invert(int64_t texh) {
    VayuTexture* t = (VayuTexture*)texh;
    if (!t || t->level_count < 1) return;
    VayuTexLevel* L = &t->levels[0];
    int64_t n = (int64_t)L->w * (int64_t)L->h;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t c = L->pixels[i];
        uint32_t a = (c >> 24) & 0xFF;
        uint32_t r = 255u - ((c >> 16) & 0xFF);
        uint32_t g = 255u - ((c >>  8) & 0xFF);
        uint32_t b = 255u - ( c        & 0xFF);
        L->pixels[i] = (a<<24) | (r<<16) | (g<<8) | b;
    }
}

void vayu_raster_post_tint(int64_t texh, int64_t argb) {
    VayuTexture* t = (VayuTexture*)texh;
    if (!t || t->level_count < 1) return;
    VayuTexLevel* L = &t->levels[0];
    int64_t n = (int64_t)L->w * (int64_t)L->h;
    uint32_t tr = (uint32_t)((argb >> 16) & 0xFF);
    uint32_t tg = (uint32_t)((argb >>  8) & 0xFF);
    uint32_t tb = (uint32_t)((argb      ) & 0xFF);
    for (int64_t i = 0; i < n; ++i) {
        uint32_t c = L->pixels[i];
        uint32_t a = (c >> 24) & 0xFF;
        uint32_t r = ((c >> 16) & 0xFF) * tr / 255u;
        uint32_t g = ((c >>  8) & 0xFF) * tg / 255u;
        uint32_t b = ( c        & 0xFF) * tb / 255u;
        L->pixels[i] = (a<<24) | (r<<16) | (g<<8) | b;
    }
}

void vayu_raster_post_brightness(int64_t texh, int64_t delta) {
    VayuTexture* t = (VayuTexture*)texh;
    if (!t || t->level_count < 1) return;
    VayuTexLevel* L = &t->levels[0];
    int64_t n = (int64_t)L->w * (int64_t)L->h;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t c = L->pixels[i];
        int a = (int)((c >> 24) & 0xFF);
        int r = (int)((c >> 16) & 0xFF) + (int)delta;
        int g = (int)((c >>  8) & 0xFF) + (int)delta;
        int b = (int)( c        & 0xFF) + (int)delta;
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        L->pixels[i] = ((uint32_t)a<<24) | ((uint32_t)r<<16)
                     | ((uint32_t)g<<8)   |  (uint32_t)b;
    }
}

void vayu_raster_post_threshold(int64_t texh, int64_t threshold) {
    VayuTexture* t = (VayuTexture*)texh;
    if (!t || t->level_count < 1) return;
    VayuTexLevel* L = &t->levels[0];
    int64_t n = (int64_t)L->w * (int64_t)L->h;
    int th = (int)threshold;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t c = L->pixels[i];
        uint32_t a = (c >> 24) & 0xFF;
        int lum = ((int)((c >> 16) & 0xFF) * 30
                 + (int)((c >>  8) & 0xFF) * 59
                 + (int)( c        & 0xFF) * 11) / 100;
        uint32_t v = (lum >= th) ? 255u : 0u;
        L->pixels[i] = (a<<24) | (v<<16) | (v<<8) | v;
    }
}

int64_t vayu_raster_tex_from_fb(int64_t fbh) {
    VayuFramebuffer* fb = (VayuFramebuffer*)fbh;
    if (!fb) return 0;
    int64_t t = vayu_raster_tex_new(fb->w, fb->h);
    if (!t) return 0;
    VayuTexture* tt = (VayuTexture*)t;
    memcpy(tt->levels[0].pixels, fb->pixels,
           sizeof(uint32_t) * (size_t)(fb->w * fb->h));
    return t;
}

/* ===========================================================================
 * Phase 22.0 - Tensor core.
 * All values are Q16.16 fixed-point, stored as int64 (values are small
 * multiples of 65536).  Row-major, dense, up to 8 dims.
 * Handle is opaque - callers must only pass it back into tensor.* funcs.
 * ========================================================================= */

typedef struct {
    double*  data;    /* Phase 24.2 - real doubles, not Q16.16 */
    int64_t* shape;
    int64_t* strides;
    int      ndim;
    int64_t  numel;
    int      requires_grad;
    int64_t  grad;
    int      tape_idx;
} VayuTensor;

/* Phase 22.1 op codes, stored on the tape entry. */
#define VAYU_OP_ADD        1
#define VAYU_OP_SUB        2
#define VAYU_OP_MUL        3
#define VAYU_OP_DIV        4
#define VAYU_OP_MATMUL     5
#define VAYU_OP_SUM        6
#define VAYU_OP_MUL_SCALAR 7
#define VAYU_OP_ADD_SCALAR 8
#define VAYU_OP_RELU       9
#define VAYU_OP_SIGMOID    10
#define VAYU_OP_TANH       11
#define VAYU_OP_EXP        12
#define VAYU_OP_LOG        13
#define VAYU_OP_TRANSPOSE  14

/* Backward-recursion guard.  Declared at the top of the tensor block so
   every tensor op (which appears before the tape-push definition below)
   can consult it.  The backward loop flips it on/off. */
static int g_in_backward = 0;

/* Forward decl - full definition lands after tensor_print. */
static void vayu_tape_push(int op, int64_t out, int64_t a, int64_t b, int64_t scalar);

static int64_t vayu_tensor_numel(const int64_t* shape, int ndim) {
    int64_t n = 1;
    for (int i = 0; i < ndim; ++i) n *= shape[i];
    return n;
}

static void vayu_tensor_strides(const int64_t* shape, int ndim, int64_t* strides) {
    if (ndim == 0) return;
    strides[ndim - 1] = 1;
    for (int i = ndim - 2; i >= 0; --i)
        strides[i] = strides[i + 1] * shape[i + 1];
}

static int64_t vayu_tensor_new_from_shape(const int64_t* shape, int ndim) {
    if (ndim < 0 || ndim > 8) return 0;
    VayuTensor* t = (VayuTensor*)malloc(sizeof(VayuTensor));
    if (!t) return 0;
    t->ndim = ndim;
    t->requires_grad = 0;
    t->grad = 0;
    t->tape_idx = -1;
    if (ndim == 0) {
        t->shape = NULL;
        t->strides = NULL;
        t->numel = 1;
    } else {
        t->shape   = (int64_t*)malloc(sizeof(int64_t) * (size_t)ndim);
        t->strides = (int64_t*)malloc(sizeof(int64_t) * (size_t)ndim);
        if (!t->shape || !t->strides) {
            free(t->shape); free(t->strides); free(t); return 0;
        }
        for (int i = 0; i < ndim; ++i) t->shape[i] = shape[i];
        vayu_tensor_strides(t->shape, ndim, t->strides);
        t->numel = vayu_tensor_numel(t->shape, ndim);
    }
    t->data = (double*)malloc(sizeof(double) * (size_t)(t->numel > 0 ? t->numel : 1));
    if (!t->data) {
        free(t->shape); free(t->strides); free(t);
        return 0;
    }
    memset(t->data, 0, sizeof(double) * (size_t)(t->numel > 0 ? t->numel : 1));
    return (int64_t)t;
}

void vayu_tensor_free(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return;
    if (t->data)    free(t->data);
    if (t->shape)   free(t->shape);
    if (t->strides) free(t->strides);
    free(t);
}

int64_t vayu_tensor_ndim(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    return t ? (int64_t)t->ndim : 0;
}
int64_t vayu_tensor_shape(int64_t h, int64_t i) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t || i < 0 || i >= t->ndim) return 0;
    return t->shape[i];
}
int64_t vayu_tensor_numel_h(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    return t ? t->numel : 0;
}
int64_t vayu_tensor_get_flat(int64_t h, int64_t i) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t || i < 0 || i >= t->numel) return 0;
    return (int64_t)(t->data[i] * 65536.0 + (t->data[i] < 0.0 ? -0.5 : 0.5));
}
void vayu_tensor_set_flat(int64_t h, int64_t i, int64_t v) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t || i < 0 || i >= t->numel) return;
    t->data[i] = (double)v / 65536.0;
}

static int vayu_tensor_read_shape(VayuList* lst, int64_t* out_shape, int* out_ndim) {
    if (!lst || lst->len <= 0 || lst->len > 8) return 0;
    for (int64_t i = 0; i < lst->len; ++i) {
        int64_t d = lst->items[i];
        if (d <= 0) return 0;
        out_shape[i] = d;
    }
    *out_ndim = (int)lst->len;
    return 1;
}

int64_t vayu_tensor_new(int64_t shape_list) {
    int64_t sh[8]; int nd = 0;
    if (!vayu_tensor_read_shape((VayuList*)shape_list, sh, &nd)) return 0;
    return vayu_tensor_new_from_shape(sh, nd);
}
int64_t vayu_tensor_zeros(int64_t shape_list) {
    return vayu_tensor_new(shape_list);
}
int64_t vayu_tensor_ones(int64_t shape_list) {
    int64_t h = vayu_tensor_new(shape_list);
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    for (int64_t i = 0; i < t->numel; ++i) t->data[i] = 1.0;
    return h;
}
int64_t vayu_tensor_from_int_list(int64_t shape_list, int64_t values_list) {
    int64_t sh[8]; int nd = 0;
    if (!vayu_tensor_read_shape((VayuList*)shape_list, sh, &nd)) return 0;
    VayuList* vl = (VayuList*)values_list;
    if (!vl) return 0;
    int64_t h = vayu_tensor_new_from_shape(sh, nd);
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    int64_t n = t->numel < vl->len ? t->numel : vl->len;
    /* Q16.16 compatibility: values[i]/65536 -> real value. */
    for (int64_t i = 0; i < n; ++i)
        t->data[i] = (double)vl->items[i] / 65536.0;
    return h;
}
int64_t vayu_tensor_from_float_list(int64_t shape_list, int64_t values_list) {
    int64_t sh[8]; int nd = 0;
    if (!vayu_tensor_read_shape((VayuList*)shape_list, sh, &nd)) return 0;
    VayuList* vl = (VayuList*)values_list;
    if (!vl) return 0;
    int64_t h = vayu_tensor_new_from_shape(sh, nd);
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    int64_t n = t->numel < vl->len ? t->numel : vl->len;
    for (int64_t i = 0; i < n; ++i)
        t->data[i] = vayu_bits_to_double(vl->items[i]);
    return h;
}
int64_t vayu_tensor_copy(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    int64_t nh = vayu_tensor_new_from_shape(t->shape, t->ndim);
    VayuTensor* nt = (VayuTensor*)nh;
    if (!nt) return 0;
    memcpy(nt->data, t->data, sizeof(double) * (size_t)t->numel);
    return nh;
}
void vayu_tensor_fill(int64_t h, int64_t v) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return;
    double vf = (double)v / 65536.0;
    for (int64_t i = 0; i < t->numel; ++i) t->data[i] = vf;
}

/* Broadcasting: align right; size-1 dims propagate; matched dims required. */
static int vayu_tensor_can_broadcast_to(VayuTensor* t, VayuTensor* out) {
    int off = out->ndim - t->ndim;
    if (off < 0) return 0;
    for (int i = 0; i < t->ndim; ++i) {
        int64_t td = t->shape[i];
        int64_t od = out->shape[i + off];
        if (td != 1 && td != od) return 0;
    }
    return 1;
}

static void vayu_tensor_broadcast_shape(VayuTensor* a, VayuTensor* b,
                                        int64_t* out_shape, int* out_ndim) {
    int nd = a->ndim > b->ndim ? a->ndim : b->ndim;
    for (int i = 0; i < nd; ++i) {
        int ia = a->ndim - nd + i;
        int ib = b->ndim - nd + i;
        int64_t da = ia >= 0 ? a->shape[ia] : 1;
        int64_t db = ib >= 0 ? b->shape[ib] : 1;
        if (da == db)                     out_shape[i] = da;
        else if (da == 1)                 out_shape[i] = db;
        else if (db == 1)                 out_shape[i] = da;
        else { out_shape[i] = 0; return; }   /* incompatible */
    }
    *out_ndim = nd;
}

static void vayu_tensor_bcast_index(VayuTensor* t, int64_t out_idx,
                                    int out_ndim, const int64_t* rem,
                                    int64_t* out_offset) {
    int off = out_ndim - t->ndim;
    int64_t idx[8];
    int64_t r = out_idx;
    for (int i = out_ndim - 1; i >= 0; --i) {
        idx[i] = r % rem[i];
        r /= rem[i];
    }
    int64_t flat = 0;
    for (int i = 0; i < t->ndim; ++i) {
        int64_t ix = idx[i + off];
        if (t->shape[i] == 1) ix = 0;
        flat += ix * t->strides[i];
    }
    *out_offset = flat;
}

static int64_t vayu_tensor_ew(int64_t a_h, int64_t b_h, int op) {
    /* op: 0=add 1=sub 2=mul 3=div */
    VayuTensor* a = (VayuTensor*)a_h;
    VayuTensor* b = (VayuTensor*)b_h;
    if (!a || !b) return 0;
    int64_t out_shape[8];
    int out_ndim = 0;
    vayu_tensor_broadcast_shape(a, b, out_shape, &out_ndim);
    for (int i = 0; i < out_ndim; ++i) if (out_shape[i] == 0) return 0;
    int64_t oh = vayu_tensor_new_from_shape(out_shape, out_ndim);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    if (!vayu_tensor_can_broadcast_to(a, out) ||
        !vayu_tensor_can_broadcast_to(b, out)) {
        vayu_tensor_free(oh);
        return 0;
    }
    for (int64_t i = 0; i < out->numel; ++i) {
        int64_t ao = 0, bo = 0;
        if (out_ndim > 0) {
            vayu_tensor_bcast_index(a, i, out_ndim, out_shape, &ao);
            vayu_tensor_bcast_index(b, i, out_ndim, out_shape, &bo);
        }
        double av = a->data[ao], bv = b->data[bo];
        double r = 0.0;
        switch (op) {
            case 0: r = av + bv; break;
            case 1: r = av - bv; break;
            case 2: r = av * bv; break;
            case 3: r = (bv == 0.0) ? 0.0 : (av / bv); break;
        }
        out->data[i] = r;
    }
    if (a->requires_grad || b->requires_grad) {
        out->requires_grad = 1;
        vayu_tape_push(VAYU_OP_ADD + op, oh, a_h, b_h, 0);
    }
    return oh;
}

int64_t vayu_tensor_add(int64_t a, int64_t b) { return vayu_tensor_ew(a, b, 0); }
int64_t vayu_tensor_sub(int64_t a, int64_t b) { return vayu_tensor_ew(a, b, 1); }
int64_t vayu_tensor_mul(int64_t a, int64_t b) { return vayu_tensor_ew(a, b, 2); }
int64_t vayu_tensor_div(int64_t a, int64_t b) { return vayu_tensor_ew(a, b, 3); }

int64_t vayu_tensor_add_scalar(int64_t a_h, int64_t v) {
    VayuTensor* a = (VayuTensor*)a_h;
    if (!a) return 0;
    int64_t oh = vayu_tensor_new_from_shape(a->shape, a->ndim);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    double vf = (double)v / 65536.0;
    for (int64_t i = 0; i < a->numel; ++i) out->data[i] = a->data[i] + vf;
    if (a->requires_grad) {
        out->requires_grad = 1;
        vayu_tape_push(VAYU_OP_ADD_SCALAR, oh, a_h, 0, v);
    }
    return oh;
}
int64_t vayu_tensor_mul_scalar(int64_t a_h, int64_t v) {
    VayuTensor* a = (VayuTensor*)a_h;
    if (!a) return 0;
    int64_t oh = vayu_tensor_new_from_shape(a->shape, a->ndim);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    double vf = (double)v / 65536.0;
    for (int64_t i = 0; i < a->numel; ++i)
        out->data[i] = a->data[i] * vf;
    if (a->requires_grad) {
        out->requires_grad = 1;
        vayu_tape_push(VAYU_OP_MUL_SCALAR, oh, a_h, 0, v);
    }
    return oh;
}

int64_t vayu_tensor_matmul(int64_t a_h, int64_t b_h) {
    VayuTensor* a = (VayuTensor*)a_h;
    VayuTensor* b = (VayuTensor*)b_h;
    if (!a || !b) return 0;
    if (a->ndim != 2 || b->ndim != 2) return 0;
    if (a->shape[1] != b->shape[0]) return 0;
    int64_t M = a->shape[0], K = a->shape[1], N = b->shape[1];
    int64_t out_shape[2] = { M, N };
    int64_t oh = vayu_tensor_new_from_shape(out_shape, 2);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    const double* A = a->data;
    const double* B = b->data;
    double* C = out->data;
    for (int64_t i = 0; i < M; ++i) {
        for (int64_t j = 0; j < N; ++j) {
            double s = 0.0;
            for (int64_t k = 0; k < K; ++k) {
                s += A[i*K + k] * B[k*N + j];
            }
            C[i*N + j] = s;
        }
    }
    if (a->requires_grad || b->requires_grad) {
        out->requires_grad = 1;
        vayu_tape_push(VAYU_OP_MATMUL, oh, a_h, b_h, 0);
    }
    return oh;
}

int64_t vayu_tensor_sum(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    double s = 0.0;
    for (int64_t i = 0; i < t->numel; ++i) s += t->data[i];
    int64_t sh[1] = { 1 };
    int64_t oh = vayu_tensor_new_from_shape(sh, 1);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    out->data[0] = s;
    if (t->requires_grad) {
        out->requires_grad = 1;
        vayu_tape_push(VAYU_OP_SUM, oh, h, 0, 0);
    }
    return oh;
}
int64_t vayu_tensor_max(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t || t->numel == 0) return 0;
    double m = t->data[0];
    for (int64_t i = 1; i < t->numel; ++i) if (t->data[i] > m) m = t->data[i];
    int64_t sh[1] = { 1 };
    int64_t oh = vayu_tensor_new_from_shape(sh, 1);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    out->data[0] = m;
    return oh;
}
int64_t vayu_tensor_argmax(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t || t->numel == 0) return -1;
    int64_t best = 0;
    for (int64_t i = 1; i < t->numel; ++i)
        if (t->data[i] > t->data[best]) best = i;
    return best;
}

int64_t vayu_tensor_reshape(int64_t h, int64_t shape_list) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    int64_t sh[8]; int nd = 0;
    if (!vayu_tensor_read_shape((VayuList*)shape_list, sh, &nd)) return 0;
    int64_t n = vayu_tensor_numel(sh, nd);
    if (n != t->numel) return 0;
    int64_t oh = vayu_tensor_new_from_shape(sh, nd);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    memcpy(out->data, t->data, sizeof(double) * (size_t)n);
    return oh;
}

int64_t vayu_tensor_transpose(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t || t->ndim != 2) return 0;
    int64_t M = t->shape[0], N = t->shape[1];
    int64_t sh[2] = { N, M };
    int64_t oh = vayu_tensor_new_from_shape(sh, 2);
    VayuTensor* out = (VayuTensor*)oh;
    if (!out) return 0;
    for (int64_t i = 0; i < M; ++i)
        for (int64_t j = 0; j < N; ++j)
            out->data[j*M + i] = t->data[i*N + j];
    if (t->requires_grad && !g_in_backward) {
        out->requires_grad = 1;
        vayu_tape_push(VAYU_OP_TRANSPOSE, oh, h, 0, 0);
    }
    return oh;
}

static void vayu_tensor_print_d(double d) {
    printf("%.4f", d);
}

void vayu_tensor_print(int64_t h, int64_t name_sp) {
    VayuTensor* t = (VayuTensor*)h;
    VayuStr* name = (VayuStr*)name_sp;
    if (name) fwrite(name->data, 1, (size_t)name->len, stdout);
    else      printf("tensor");
    printf(" shape=[");
    for (int i = 0; i < t->ndim; ++i) {
        if (i) printf(",");
        printf("%lld", (long long)t->shape[i]);
    }
    printf("] data=[");
    for (int64_t i = 0; i < t->numel; ++i) {
        if (i) printf(" ");
        vayu_tensor_print_d(t->data[i]);
    }
    printf("]\n");
}

/* ===========================================================================
 * Phase 22.1 - autodiff tape + transcendentals + reverse-mode backward.
 * ========================================================================= */

typedef struct {
    int      op;
    int64_t  out, a, b;
    int64_t  scalar;
} VayuTapeEntry;

static VayuTapeEntry* g_tape = NULL;
static int g_tape_len = 0;
static int g_tape_cap = 0;

static void vayu_tape_push(int op, int64_t out, int64_t a, int64_t b, int64_t scalar) {
    /* Backward runs transient ops on the same tensor handles; without this
       guard every backward step would grow the tape and realloc g_tape
       mid-iteration, invalidating the entry pointer in the loop. */
    if (g_in_backward) return;
    if (g_tape_len >= g_tape_cap) {
        g_tape_cap = g_tape_cap == 0 ? 256 : g_tape_cap * 2;
        g_tape = (VayuTapeEntry*)realloc(g_tape,
                                         sizeof(VayuTapeEntry) * (size_t)g_tape_cap);
    }
    g_tape[g_tape_len].op = op;
    g_tape[g_tape_len].out = out;
    g_tape[g_tape_len].a = a;
    g_tape[g_tape_len].b = b;
    g_tape[g_tape_len].scalar = scalar;
    if (out) ((VayuTensor*)out)->tape_idx = g_tape_len;
    g_tape_len++;
}

void vayu_tensor_tape_clear(void) { g_tape_len = 0; }
int64_t vayu_tensor_tape_size(void) { return (int64_t)g_tape_len; }

void vayu_tensor_requires_grad(int64_t h, int64_t flag) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return;
    t->requires_grad = flag ? 1 : 0;
}
void vayu_tensor_zero_grad(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return;
    t->grad = 0;
}
int64_t vayu_tensor_grad(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    return t ? t->grad : 0;
}
void vayu_tensor_copy_into(int64_t dst_h, int64_t src_h) {
    VayuTensor* d = (VayuTensor*)dst_h;
    VayuTensor* s = (VayuTensor*)src_h;
    if (!d || !s) return;
    if (d->numel != s->numel) return;
    memcpy(d->data, s->data, sizeof(int64_t) * (size_t)d->numel);
}

/* Q16.16 transcendentals.  Round-trip through double for accuracy. */
static int64_t vayu_q_mul(int64_t a, int64_t b) { return (a * b) >> 16; }
static int64_t vayu_q_exp(int64_t x) {
    double d = exp((double)x / 65536.0);
    return (int64_t)(d * 65536.0 + 0.5);
}
static int64_t vayu_q_log(int64_t x) {
    if (x <= 0) return -100LL * 65536;
    double d = log((double)x / 65536.0);
    return (int64_t)(d * 65536.0);
}
static int64_t vayu_q_tanh(int64_t x) {
    double d = tanh((double)x / 65536.0);
    return (int64_t)(d * 65536.0 + (d < 0.0 ? -0.5 : 0.5));
}
static int64_t vayu_q_sigmoid(int64_t x) {
    double d = 1.0 / (1.0 + exp(-(double)x / 65536.0));
    return (int64_t)(d * 65536.0 + 0.5);
}
static int64_t vayu_q_relu(int64_t x) { return x > 0 ? x : 0; }

static int64_t vayu_tensor_unary(int64_t h, int op) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    int64_t r = vayu_tensor_new_from_shape(t->shape, t->ndim);
    VayuTensor* o = (VayuTensor*)r;
    if (!o) return 0;
    for (int64_t i = 0; i < t->numel; ++i) {
        double x = t->data[i];
        double y = 0.0;
        switch (op) {
            case VAYU_OP_RELU:    y = (x > 0.0) ? x : 0.0;                 break;
            case VAYU_OP_SIGMOID: y = 1.0 / (1.0 + exp(-x));               break;
            case VAYU_OP_TANH:    y = tanh(x);                             break;
            case VAYU_OP_EXP:     y = exp(x);                              break;
            case VAYU_OP_LOG:     y = (x > 0.0) ? log(x) : -1.0e30;        break;
        }
        o->data[i] = y;
    }
    if (t->requires_grad) {
        o->requires_grad = 1;
        vayu_tape_push(op, r, h, 0, 0);
    }
    return r;
}

int64_t vayu_tensor_relu(int64_t h)    { return vayu_tensor_unary(h, VAYU_OP_RELU); }
int64_t vayu_tensor_sigmoid(int64_t h) { return vayu_tensor_unary(h, VAYU_OP_SIGMOID); }
int64_t vayu_tensor_tanh(int64_t h)    { return vayu_tensor_unary(h, VAYU_OP_TANH); }
int64_t vayu_tensor_exp(int64_t h)     { return vayu_tensor_unary(h, VAYU_OP_EXP); }
int64_t vayu_tensor_log(int64_t h)     { return vayu_tensor_unary(h, VAYU_OP_LOG); }

/* --- Backward helpers --- */

static int64_t vayu_tensor_ones_like(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    int64_t r = vayu_tensor_new_from_shape(t->shape, t->ndim);
    VayuTensor* o = (VayuTensor*)r;
    for (int64_t i = 0; i < o->numel; ++i) o->data[i] = 1.0;
    return r;
}

static int64_t vayu_tensor_neg(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return 0;
    int64_t r = vayu_tensor_new_from_shape(t->shape, t->ndim);
    VayuTensor* o = (VayuTensor*)r;
    for (int64_t i = 0; i < t->numel; ++i) o->data[i] = -t->data[i];
    return r;
}

static int64_t vayu_tensor_ones_shape(const int64_t* shape, int ndim) {
    int64_t r = vayu_tensor_new_from_shape(shape, ndim);
    VayuTensor* o = (VayuTensor*)r;
    if (!o) return 0;
    for (int64_t i = 0; i < o->numel; ++i) o->data[i] = 65536;
    return r;
}

static int64_t vayu_tensor_reduce_to_shape(int64_t d_h, VayuTensor* target) {
    VayuTensor* d = (VayuTensor*)d_h;
    if (!d) return 0;
    int same = (d->ndim == target->ndim);
    if (same) {
        for (int i = 0; i < d->ndim; ++i)
            if (d->shape[i] != target->shape[i]) { same = 0; break; }
    }
    if (same) return vayu_tensor_copy(d_h);
    int64_t oh = vayu_tensor_new_from_shape(target->shape, target->ndim);
    VayuTensor* o = (VayuTensor*)oh;
    if (!o) return 0;
    int off = d->ndim - target->ndim;
    if (off < 0) { vayu_tensor_free(oh); return vayu_tensor_copy(d_h); }
    for (int64_t i = 0; i < d->numel; ++i) {
        int64_t idx[8];
        int64_t r = i;
        for (int k = d->ndim - 1; k >= 0; --k) { idx[k] = r % d->shape[k]; r /= d->shape[k]; }
        int64_t o_flat = 0;
        for (int k = 0; k < target->ndim; ++k) {
            int64_t ix = idx[k + off];
            if (target->shape[k] == 1) ix = 0;
            o_flat = o_flat * target->shape[k] + ix;
        }
        o->data[o_flat] += d->data[i];
    }
    return oh;
}

static void vayu_grad_accum(int64_t h, int64_t d_h) {
    if (!h || !d_h) return;
    VayuTensor* t = (VayuTensor*)h;
    if (!t->requires_grad) return;
    VayuTensor* dt = (VayuTensor*)d_h;
    int mismatch = (dt->ndim != t->ndim);
    if (!mismatch) {
        for (int i = 0; i < t->ndim; ++i)
            if (dt->shape[i] != t->shape[i]) { mismatch = 1; break; }
    }
    if (mismatch) {
        d_h = vayu_tensor_reduce_to_shape(d_h, t);
        if (!d_h) return;
    }
    if (!t->grad) { t->grad = d_h; return; }
    int64_t sum = vayu_tensor_add(t->grad, d_h);
    t->grad = sum;
}

static int64_t vayu_unary_local(int op, VayuTensor* x, VayuTensor* y, int64_t gi) {
    int64_t xi = x ? 0 : 0, yi = 0;
    (void)xi;
    if (x) xi = 0;
    int64_t d = 0;
    (void)yi;
    switch (op) {
        case VAYU_OP_RELU:    d = (x && x->data[0] > 0) ? 65536 : 0; break;
        case VAYU_OP_SIGMOID: d = vayu_q_mul(y->data[0], 65536 - y->data[0]); break;
        case VAYU_OP_TANH:    d = 65536 - vayu_q_mul(y->data[0], y->data[0]); break;
        case VAYU_OP_EXP:     d = y->data[0]; break;
        case VAYU_OP_LOG:     d = x->data[0] > 0 ? (int64_t)(65536.0 * 65536.0 / (double)x->data[0]) : 0; break;
    }
    return vayu_q_mul(gi, d);
}

/* ===========================================================================
 * Phase 22.3 - .vnn model loading + graph execution.
 * ========================================================================= */

typedef struct {
    char        name[64];
    VayuTensor* t;
} VayuNNBind;

typedef struct {
    VayuNNBind binds[64];
    int        n_binds;
    char       input_name[64];
} VayuNNModel;

static int vayu_nn_lookup(VayuNNModel* m, const char* name) {
    for (int i = 0; i < m->n_binds; ++i)
        if (strcmp(m->binds[i].name, name) == 0) return i;
    return -1;
}
static void vayu_nn_bind(VayuNNModel* m, const char* name, VayuTensor* t) {
    int idx = vayu_nn_lookup(m, name);
    if (idx >= 0) { m->binds[idx].t = t; return; }
    if (m->n_binds >= 64) return;
    int n = (int)strlen(name);
    if (n > 63) n = 63;
    memcpy(m->binds[m->n_binds].name, name, (size_t)n);
    m->binds[m->n_binds].name[n] = 0;
    m->binds[m->n_binds].t = t;
    m->n_binds++;
}
static void vayu_nn_skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t' || **p == '\r') (*p)++;
}
static int vayu_nn_read_word(const char** p, char* out, int cap) {
    vayu_nn_skip_ws(p);
    int n = 0;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && **p != '\r') {
        if (n < cap - 1) out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    return n > 0;
}
static int64_t vayu_nn_read_int(const char** p) {
    vayu_nn_skip_ws(p);
    int neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    int64_t v = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return neg ? -v : v;
}

/* ===========================================================================
 * Phase 22.3b - ONNX protobuf decoder + graph executor.
 * Minimal wire decoder (no libprotobuf).  Handles the ONNX subset needed
 * for MLP-scale inference: Gemm, MatMul, Add, Mul, Sub, Div, Relu,
 * Sigmoid, Tanh, Softmax, Flatten, Transpose.
 * ========================================================================= */

typedef struct { const uint8_t* p; const uint8_t* end; } PbReader;

static int pb_read_byte(PbReader* r, uint8_t* out) {
    if (r->p >= r->end) return 0;
    *out = *r->p++;
    return 1;
}
static int pb_read_varint(PbReader* r, uint64_t* out) {
    uint64_t v = 0;
    int shift = 0;
    while (shift < 64) {
        uint8_t b;
        if (!pb_read_byte(r, &b)) return 0;
        v |= ((uint64_t)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) { *out = v; return 1; }
        shift += 7;
    }
    return 0;
}
static int pb_read_tag(PbReader* r, int* field, int* wire) {
    uint64_t k;
    if (!pb_read_varint(r, &k)) return 0;
    *field = (int)(k >> 3);
    *wire  = (int)(k & 7);
    return 1;
}
static int pb_skip(PbReader* r, int wire) {
    uint64_t v;
    switch (wire) {
        case 0: return pb_read_varint(r, &v);
        case 1: r->p += 8; return r->p <= r->end;
        case 2: if (!pb_read_varint(r, &v)) return 0;
                r->p += v; return r->p <= r->end;
        case 5: r->p += 4; return r->p <= r->end;
    }
    return 0;
}
static int pb_read_len(PbReader* r, const uint8_t** out, size_t* len) {
    uint64_t v;
    if (!pb_read_varint(r, &v)) return 0;
    if (r->p + v > r->end) return 0;
    *out = r->p;
    *len = (size_t)v;
    r->p += v;
    return 1;
}

/* ---------- Model representation ---------- */

#define ONNX_MAX_BIND   128
#define ONNX_MAX_NODE   128
#define ONNX_MAX_IN      4
#define ONNX_MAX_OUT     1
#define ONNX_NAME_CAP   64

typedef struct { char name[ONNX_NAME_CAP]; int64_t t; } OnnxBind;

typedef struct {
    char    op[24];
    char    inputs[ONNX_MAX_IN][ONNX_NAME_CAP];
    int     n_inputs;
    char    output[ONNX_NAME_CAP];
    int64_t alpha_q;   /* Gemm default 1.0 */
    int64_t beta_q;    /* Gemm default 1.0 */
    int     transA;
    int     transB;
    int     axis;      /* Flatten default 1 */
} OnnxNode;

typedef struct {
    OnnxBind binds[ONNX_MAX_BIND];
    int      n_binds;
    OnnxNode nodes[ONNX_MAX_NODE];
    int      n_nodes;
    char     inputs[ONNX_MAX_IN][ONNX_NAME_CAP];
    int      n_inputs;
    char     outputs[ONNX_MAX_OUT][ONNX_NAME_CAP];
    int      n_outputs;
} VayuOnnx;

static int onnx_bind_find(VayuOnnx* m, const char* name) {
    for (int i = 0; i < m->n_binds; ++i)
        if (strcmp(m->binds[i].name, name) == 0) return i;
    return -1;
}
static void onnx_bind_set(VayuOnnx* m, const char* name, int64_t t) {
    int idx = onnx_bind_find(m, name);
    if (idx >= 0) { m->binds[idx].t = t; return; }
    if (m->n_binds >= ONNX_MAX_BIND) return;
    int n = (int)strlen(name);
    if (n > ONNX_NAME_CAP - 1) n = ONNX_NAME_CAP - 1;
    memcpy(m->binds[m->n_binds].name, name, (size_t)n);
    m->binds[m->n_binds].name[n] = 0;
    m->binds[m->n_binds].t = t;
    m->n_binds++;
}
static int64_t onnx_bind_get(VayuOnnx* m, const char* name) {
    int idx = onnx_bind_find(m, name);
    return idx >= 0 ? m->binds[idx].t : 0;
}

/* ---------- TensorProto ---------- */
static void onnx_parse_tensor(const uint8_t* data, size_t len, VayuOnnx* m) {
    PbReader r = { data, data + len };
    int64_t dims[8]; int ndim = 0;
    int dtype = 0;
    char name[ONNX_NAME_CAP]; name[0] = 0;
    const uint8_t* raw = NULL; size_t raw_len = 0;
    const uint8_t* fpacked = NULL; size_t fpacked_len = 0;
    const uint8_t* ipacked = NULL; size_t ipacked_len = 0;

    while (r.p < r.end) {
        int f, w;
        if (!pb_read_tag(&r, &f, &w)) break;
        if (f == 1 && w == 0) { uint64_t v; if (!pb_read_varint(&r, &v)) break;
                                 if (ndim < 8) dims[ndim++] = (int64_t)v; }
        else if (f == 1 && w == 2) {
            const uint8_t* b; size_t bl; if (!pb_read_len(&r, &b, &bl)) break;
            PbReader rr = { b, b + bl };
            while (rr.p < rr.end) { uint64_t v; if (!pb_read_varint(&rr, &v)) break;
                                    if (ndim < 8) dims[ndim++] = (int64_t)v; }
        }
        else if (f == 2 && w == 0) { uint64_t v; if (!pb_read_varint(&r, &v)) break;
                                     dtype = (int)v; }
        else if (f == 4 && w == 2) { if (!pb_read_len(&r, &fpacked, &fpacked_len)) break; }
        else if (f == 7 && w == 2) { if (!pb_read_len(&r, &ipacked, &ipacked_len)) break; }
        else if (f == 8 && w == 2) {
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            int cp = (int)(bl < ONNX_NAME_CAP - 1 ? bl : ONNX_NAME_CAP - 1);
            memcpy(name, b, (size_t)cp); name[cp] = 0;
        }
        else if (f == 9 && w == 2) { if (!pb_read_len(&r, &raw, &raw_len)) break; }
        else if (!pb_skip(&r, w)) break;
    }

    if (ndim == 0 || name[0] == 0) return;
    int64_t sh[8];
    for (int i = 0; i < ndim; ++i) sh[i] = dims[i];
    int64_t th = vayu_tensor_new_from_shape(sh, ndim);
    VayuTensor* t = (VayuTensor*)th;
    if (!t) return;

    if (dtype == 1) {  /* FLOAT */
        const uint8_t* src = raw ? raw : fpacked;
        size_t slen = raw ? raw_len : fpacked_len;
        if (src && slen >= (size_t)t->numel * 4) {
            for (int64_t i = 0; i < t->numel; ++i) {
                float fv; memcpy(&fv, src + i * 4, 4);
                t->data[i] = (int64_t)((double)fv * 65536.0 + (fv < 0 ? -0.5 : 0.5));
            }
        }
    } else if (dtype == 7) {  /* INT64 */
        if (raw && raw_len >= (size_t)t->numel * 8) {
            for (int64_t i = 0; i < t->numel; ++i) {
                int64_t iv; memcpy(&iv, raw + i * 8, 8);
                t->data[i] = iv << 16;
            }
        } else if (ipacked && ipacked_len >= (size_t)t->numel * 8) {
            for (int64_t i = 0; i < t->numel; ++i) {
                int64_t iv; memcpy(&iv, ipacked + i * 8, 8);
                t->data[i] = iv << 16;
            }
        }
    }
    onnx_bind_set(m, name, (int64_t)t);
}

/* ---------- AttributeProto ---------- */
static void onnx_parse_attr(const uint8_t* data, size_t len,
                            char* name_out, int* type_out,
                            int64_t* i_out, int64_t* f_out)
{
    PbReader r = { data, data + len };
    *type_out = 0; *i_out = 0; *f_out = 0;
    name_out[0] = 0;
    while (r.p < r.end) {
        int f, w;
        if (!pb_read_tag(&r, &f, &w)) break;
        if (f == 1 && w == 2) {
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            int cp = (int)(bl < ONNX_NAME_CAP - 1 ? bl : ONNX_NAME_CAP - 1);
            memcpy(name_out, b, (size_t)cp); name_out[cp] = 0;
        } else if (f == 2 && w == 5) {   /* f (float, fixed32) */
            float fv; memcpy(&fv, r.p, 4); r.p += 4;
            *f_out = (int64_t)((double)fv * 65536.0);
        } else if (f == 3 && w == 0) {   /* i (int64 varint) */
            uint64_t v; if (!pb_read_varint(&r, &v)) break;
            *i_out = (int64_t)v;
        } else if (f == 20 && w == 0) {  /* type */
            uint64_t v; if (!pb_read_varint(&r, &v)) break;
            *type_out = (int)v;
        } else if (!pb_skip(&r, w)) break;
    }
}

/* ---------- NodeProto ---------- */
static void onnx_parse_node(const uint8_t* data, size_t len, VayuOnnx* m) {
    if (m->n_nodes >= ONNX_MAX_NODE) return;
    OnnxNode* nd = &m->nodes[m->n_nodes];
    memset(nd, 0, sizeof(*nd));
    nd->alpha_q = 65536;
    nd->beta_q  = 65536;
    nd->axis    = 1;

    PbReader r = { data, data + len };
    while (r.p < r.end) {
        int f, w;
        if (!pb_read_tag(&r, &f, &w)) break;
        if (f == 1 && w == 2) {          /* input */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            if (nd->n_inputs < ONNX_MAX_IN) {
                int cp = (int)(bl < ONNX_NAME_CAP - 1 ? bl : ONNX_NAME_CAP - 1);
                memcpy(nd->inputs[nd->n_inputs], b, (size_t)cp);
                nd->inputs[nd->n_inputs][cp] = 0;
                nd->n_inputs++;
            }
        } else if (f == 2 && w == 2) {   /* output */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            int cp = (int)(bl < ONNX_NAME_CAP - 1 ? bl : ONNX_NAME_CAP - 1);
            memcpy(nd->output, b, (size_t)cp);
            nd->output[cp] = 0;
        } else if (f == 4 && w == 2) {   /* op_type */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            int cp = (int)(bl < 23 ? bl : 23);
            memcpy(nd->op, b, (size_t)cp);
            nd->op[cp] = 0;
        } else if (f == 5 && w == 2) {   /* attribute */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            char an[64]; int at; int64_t ai, af;
            onnx_parse_attr(b, bl, an, &at, &ai, &af);
            /* AttributeType: UNDEFINED=0 FLOAT=1 INT=2 STRING=3 TENSOR=4
               GRAPH=5 FLOATS=6 INTS=7 STRINGS=8 TENSORS=9 GRAPHS=10
               SPARSE_TENSOR=11 SPARSE_TENSORS=12 TYPE_PROTO=13 TYPE_PROTOS=14 */
            if      (strcmp(an, "alpha")  == 0 && at == 1) nd->alpha_q = af;
            else if (strcmp(an, "beta")   == 0 && at == 1) nd->beta_q  = af;
            else if (strcmp(an, "transA") == 0 && at == 2) nd->transA  = (int)ai;
            else if (strcmp(an, "transB") == 0 && at == 2) nd->transB  = (int)ai;
            else if (strcmp(an, "axis")   == 0 && at == 2) nd->axis    = (int)ai;
        } else if (!pb_skip(&r, w)) break;
    }
    if (nd->op[0] == 0 || nd->output[0] == 0) return;
    m->n_nodes++;
}

/* ---------- ValueInfoProto (only name matters for shape inference) ---------- */
static void onnx_parse_value_info(const uint8_t* data, size_t len,
                                  char* name_out, int cap)
{
    PbReader r = { data, data + len };
    name_out[0] = 0;
    while (r.p < r.end) {
        int f, w;
        if (!pb_read_tag(&r, &f, &w)) break;
        if (f == 1 && w == 2) {
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            int cp = (int)(bl < (size_t)(cap - 1) ? bl : (size_t)(cap - 1));
            memcpy(name_out, b, (size_t)cp); name_out[cp] = 0;
        } else if (!pb_skip(&r, w)) break;
    }
}

/* ---------- GraphProto ---------- */
static void onnx_parse_graph(const uint8_t* data, size_t len, VayuOnnx* m) {
    PbReader r = { data, data + len };
    while (r.p < r.end) {
        int f, w;
        if (!pb_read_tag(&r, &f, &w)) break;
        if (f == 1 && w == 2) {          /* node */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            onnx_parse_node(b, bl, m);
        } else if (f == 5 && w == 2) {   /* initializer */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            onnx_parse_tensor(b, bl, m);
        } else if (f == 11 && w == 2) {  /* input */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            if (m->n_inputs < ONNX_MAX_IN) {
                onnx_parse_value_info(b, bl,
                    m->inputs[m->n_inputs], ONNX_NAME_CAP);
                if (m->inputs[m->n_inputs][0]) m->n_inputs++;
            }
        } else if (f == 12 && w == 2) {  /* output */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            if (m->n_outputs < ONNX_MAX_OUT) {
                onnx_parse_value_info(b, bl,
                    m->outputs[m->n_outputs], ONNX_NAME_CAP);
                if (m->outputs[m->n_outputs][0]) m->n_outputs++;
            }
        } else if (!pb_skip(&r, w)) break;
    }
}

/* ---------- Softmax (numerically stabilised) ---------- */
static int64_t vayu_tensor_softmax(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t || t->numel == 0) return 0;
    double mx = t->data[0];
    for (int64_t i = 1; i < t->numel; ++i) if (t->data[i] > mx) mx = t->data[i];
    int64_t oh = vayu_tensor_new_from_shape(t->shape, t->ndim);
    VayuTensor* o = (VayuTensor*)oh;
    if (!o) return 0;
    double sum = 0.0;
    for (int64_t i = 0; i < t->numel; ++i) {
        double e = exp(t->data[i] - mx);
        o->data[i] = e;
        sum += e;
    }
    if (sum <= 0.0) return oh;
    for (int64_t i = 0; i < t->numel; ++i) o->data[i] = o->data[i] / sum;
    return oh;
}

/* ---------- Executor ---------- */
static int64_t vayu_onnx_execute(VayuOnnx* m, int64_t input_h) {
    if (m->n_inputs > 0 && input_h) onnx_bind_set(m, m->inputs[0], input_h);
    for (int i = 0; i < m->n_nodes; ++i) {
        OnnxNode* nd = &m->nodes[i];
        int64_t A = nd->n_inputs >= 1 ? onnx_bind_get(m, nd->inputs[0]) : 0;
        int64_t B = nd->n_inputs >= 2 ? onnx_bind_get(m, nd->inputs[1]) : 0;
        int64_t C = nd->n_inputs >= 3 ? onnx_bind_get(m, nd->inputs[2]) : 0;
        if (!A) return 0;

        int64_t r = 0;
        if      (strcmp(nd->op, "MatMul")   == 0) r = vayu_tensor_matmul(A, B);
        else if (strcmp(nd->op, "Gemm")     == 0) {
            int64_t A2 = nd->transA ? vayu_tensor_transpose(A) : A;
            int64_t B2 = nd->transB ? vayu_tensor_transpose(B) : B;
            int64_t mm = vayu_tensor_matmul(A2, B2);
            r = mm;
            if (nd->n_inputs >= 3 && C) {
                int64_t ba = vayu_tensor_mul_scalar(mm, nd->alpha_q);
                int64_t bb = vayu_tensor_mul_scalar(C,  nd->beta_q);
                r = vayu_tensor_add(ba, bb);
            }
        }
        else if (strcmp(nd->op, "Add")      == 0) r = vayu_tensor_add(A, B);
        else if (strcmp(nd->op, "Sub")      == 0) r = vayu_tensor_sub(A, B);
        else if (strcmp(nd->op, "Mul")      == 0) r = vayu_tensor_mul(A, B);
        else if (strcmp(nd->op, "Div")      == 0) r = vayu_tensor_div(A, B);
        else if (strcmp(nd->op, "Relu")     == 0) r = vayu_tensor_relu(A);
        else if (strcmp(nd->op, "Sigmoid")  == 0) r = vayu_tensor_sigmoid(A);
        else if (strcmp(nd->op, "Tanh")     == 0) r = vayu_tensor_tanh(A);
        else if (strcmp(nd->op, "Softmax")  == 0) r = vayu_tensor_softmax(A);
        else if (strcmp(nd->op, "Transpose")== 0) r = vayu_tensor_transpose(A);
        else if (strcmp(nd->op, "Flatten")  == 0) {
            VayuTensor* t = (VayuTensor*)A;
            if (t->ndim != 2) { r = A; }
            else {
                int64_t sh[2] = { t->shape[0], t->shape[1] };
                r = vayu_tensor_reshape(A, (int64_t)&(VayuList){0});
                /* simple 2D flatten is identity; reshape to [batch, prod] */
                int64_t nsh[8]; int nn = 0;
                nsh[nn++] = t->shape[0];
                int64_t rest = 1;
                for (int d = 1; d < t->ndim; ++d) rest *= t->shape[d];
                nsh[nn++] = rest;
                int64_t fl = vayu_tensor_new_from_shape(nsh, 2);
                memcpy(((VayuTensor*)fl)->data, t->data,
                       sizeof(int64_t) * (size_t)t->numel);
                r = fl;
                (void)sh;
            }
        }
        if (!r) return 0;
        onnx_bind_set(m, nd->output, r);
    }
    if (m->n_outputs > 0) return onnx_bind_get(m, m->outputs[0]);
    if (m->n_nodes > 0)   return onnx_bind_get(m, m->nodes[m->n_nodes-1].output);
    return 0;
}

/* ---------- Public API ---------- */

int64_t vayu_onnx_load(int64_t path_sp) {
    VayuStr* path = (VayuStr*)path_sp;
    FILE* f = fopen(path->data, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    VayuOnnx* m = (VayuOnnx*)malloc(sizeof(VayuOnnx));
    memset(m, 0, sizeof(*m));

    PbReader r = { buf, buf + sz };
    while (r.p < r.end) {
        int f2, w;
        if (!pb_read_tag(&r, &f2, &w)) break;
        if (f2 == 7 && w == 2) {  /* ModelProto.graph */
            const uint8_t* b; size_t bl;
            if (!pb_read_len(&r, &b, &bl)) break;
            onnx_parse_graph(b, bl, m);
        } else if (!pb_skip(&r, w)) break;
    }
    free(buf);
    return (int64_t)m;
}

void vayu_onnx_free(int64_t h) {
    VayuOnnx* m = (VayuOnnx*)h;
    if (!m) return;
    free(m);
}

int64_t vayu_onnx_run(int64_t model_h, int64_t input_h) {
    VayuOnnx* m = (VayuOnnx*)model_h;
    if (!m) return 0;
    return vayu_onnx_execute(m, input_h);
}

int64_t vayu_onnx_input_name(int64_t h) {
    VayuOnnx* m = (VayuOnnx*)h;
    if (!m || m->n_inputs == 0) return (int64_t)vayu_mkstr("", 0);
    return (int64_t)vayu_mkstr_c(m->inputs[0]);
}
int64_t vayu_onnx_output_name(int64_t h) {
    VayuOnnx* m = (VayuOnnx*)h;
    if (!m || m->n_outputs == 0) return (int64_t)vayu_mkstr("", 0);
    return (int64_t)vayu_mkstr_c(m->outputs[0]);
}

void vayu_onnx_debug(int64_t h) {
    VayuOnnx* m = (VayuOnnx*)h;
    if (!m) { printf("[onnx] null model\n"); return; }
    printf("[onnx] binds=%d nodes=%d inputs=%d outputs=%d\n",
           m->n_binds, m->n_nodes, m->n_inputs, m->n_outputs);
    for (int i = 0; i < m->n_inputs; ++i)
        printf("[onnx]   in  %s\n", m->inputs[i]);
    for (int i = 0; i < m->n_outputs; ++i)
        printf("[onnx]   out %s\n", m->outputs[i]);
    for (int i = 0; i < m->n_binds; ++i) {
        VayuTensor* t = (VayuTensor*)m->binds[i].t;
        if (!t) { printf("[onnx]   bind %s = NULL\n", m->binds[i].name); continue; }
        printf("[onnx]   bind %s shape=[", m->binds[i].name);
        for (int k = 0; k < t->ndim; ++k)
            printf("%s%lld", k ? "," : "", (long long)t->shape[k]);
        printf("]\n");
    }
    for (int i = 0; i < m->n_nodes; ++i) {
        OnnxNode* nd = &m->nodes[i];
        printf("[onnx]   node %d: %s  in=[", i, nd->op);
        for (int k = 0; k < nd->n_inputs; ++k)
            printf("%s%s", k ? "," : "", nd->inputs[k]);
        printf("] out=%s transB=%d\n", nd->output, nd->transB);
    }
}

/* ===========================================================================
 * Phase 22.4 - CUDA matmul via runtime NVRTC compilation.
 * Loads nvcuda + nvrtc lazily.  If either is missing, cuda.available()
 * returns 0 and every op is a no-op.  CPU fallback is the caller's job.
 * ========================================================================= */

#ifdef _WIN32

typedef void* VayuCUcontext;
typedef void* VayuCUmodule;
typedef void* VayuCUfunction;
typedef int   VayuCUresult;

typedef struct {
    void* lib_nvcuda;
    void* lib_nvrtc;
    int   inited;
    int   device_count;

    /* nvcuda driver API */
    int (*cuInit)(unsigned int);
    int (*cuDeviceGetCount)(int*);
    int (*cuDeviceGet)(int*, int);
    int (*cuCtxCreate)(VayuCUcontext*, unsigned int, int);
    int (*cuCtxDestroy)(VayuCUcontext);
    int (*cuMemAlloc)(void**, size_t);
    int (*cuMemFree)(void*);
    int (*cuMemcpyHtoD)(void*, const void*, size_t);
    int (*cuMemcpyDtoH)(void*, const void*, size_t);
    int (*cuModuleLoadData)(VayuCUmodule*, const void*);
    int (*cuModuleGetFunction)(VayuCUfunction*, VayuCUmodule, const char*);
    int (*cuLaunchKernel)(VayuCUfunction,
                          unsigned, unsigned, unsigned,
                          unsigned, unsigned, unsigned,
                          unsigned, void*, void**, void**);
    int (*cuCtxSynchronize)(void);

    /* nvrtc */
    int    (*nvrtcCreateProgram)(void**, const char*, const char*, int,
                                 const char**, const char**);
    int    (*nvrtcCompileProgram)(void*, int, const char**);
    int    (*nvrtcGetPTXSize)(void*, size_t*);
    int    (*nvrtcGetPTX)(void*, char*);
    int    (*nvrtcDestroyProgram)(void**);

    /* cached kernel */
    VayuCUfunction matmul_kernel;
    VayuCUcontext  ctx;
} VayuCuda;

static VayuCuda g_cuda;
static int     g_cuda_state = -1;   /* -1 = uninit, 0 = unavailable, 1 = ready */

static const char* VAYU_CUDA_KERNEL_SRC =
    "extern \"C\" __global__ void vayu_q16_matmul(\n"
    "    const long long* __restrict__ A,\n"
    "    const long long* __restrict__ B,\n"
    "    long long* __restrict__ C,\n"
    "    int M, int K, int N)\n"
    "{\n"
    "    int i = blockIdx.y * blockDim.y + threadIdx.y;\n"
    "    int j = blockIdx.x * blockDim.x + threadIdx.x;\n"
    "    if (i >= M || j >= N) return;\n"
    "    double s = 0.0;\n"
    "    for (int k = 0; k < K; ++k) {\n"
    "        s += ((double)A[(long long)i * K + k]\n"
    "             *(double)B[(long long)k * N + j]) / 65536.0;\n"
    "    }\n"
    "    C[(long long)i * N + j] = (long long)(s + (s < 0 ? -0.5 : 0.5));\n"
    "}\n";

static void* vayu_try_load(const char* const* names, int n) {
    for (int i = 0; i < n; ++i) {
        HMODULE h = LoadLibraryA(names[i]);
        if (h) return (void*)h;
    }
    return NULL;
}

static int vayu_cuda_init(void) {
    if (g_cuda_state >= 0) return g_cuda_state;
    memset(&g_cuda, 0, sizeof(g_cuda));

    const char* nvcuda_names[] = { "nvcuda.dll" };
    g_cuda.lib_nvcuda = vayu_try_load(nvcuda_names, 1);
    if (!g_cuda.lib_nvcuda) { g_cuda_state = 0; return 0; }

    const char* nvrtc_names[] = {
        "nvrtc.dll",
        "nvrtc64_120_0.dll", "nvrtc64_112_0.dll", "nvrtc64_111_0.dll",
        "nvrtc64_110_0.dll", "nvrtc64_102_0.dll", "nvrtc64_101_0.dll",
        "nvrtc64_100_0.dll"
    };
    g_cuda.lib_nvrtc = vayu_try_load(nvrtc_names,
        (int)(sizeof(nvrtc_names) / sizeof(nvrtc_names[0])));
    if (!g_cuda.lib_nvrtc) { g_cuda_state = 0; return 0; }

    #define GET(lib, name) do { \
        g_cuda.name = (void*)GetProcAddress((HMODULE)(lib), #name); \
        if (!g_cuda.name) { g_cuda_state = 0; return 0; } \
    } while (0)

    GET(g_cuda.lib_nvcuda, cuInit);
    GET(g_cuda.lib_nvcuda, cuDeviceGetCount);
    GET(g_cuda.lib_nvcuda, cuDeviceGet);
    GET(g_cuda.lib_nvcuda, cuCtxCreate);
    GET(g_cuda.lib_nvcuda, cuCtxDestroy);
    GET(g_cuda.lib_nvcuda, cuMemAlloc);
    GET(g_cuda.lib_nvcuda, cuMemFree);
    GET(g_cuda.lib_nvcuda, cuMemcpyHtoD);
    GET(g_cuda.lib_nvcuda, cuMemcpyDtoH);
    GET(g_cuda.lib_nvcuda, cuModuleLoadData);
    GET(g_cuda.lib_nvcuda, cuModuleGetFunction);
    GET(g_cuda.lib_nvcuda, cuLaunchKernel);
    GET(g_cuda.lib_nvcuda, cuCtxSynchronize);

    GET(g_cuda.lib_nvrtc, nvrtcCreateProgram);
    GET(g_cuda.lib_nvrtc, nvrtcCompileProgram);
    GET(g_cuda.lib_nvrtc, nvrtcGetPTXSize);
    GET(g_cuda.lib_nvrtc, nvrtcGetPTX);
    GET(g_cuda.lib_nvrtc, nvrtcDestroyProgram);
    #undef GET

    if (g_cuda.cuInit(0) != 0) { g_cuda_state = 0; return 0; }
    if (g_cuda.cuDeviceGetCount(&g_cuda.device_count) != 0 ||
        g_cuda.device_count <= 0) { g_cuda_state = 0; return 0; }

    int dev = 0;
    if (g_cuda.cuDeviceGet(&dev, 0) != 0) { g_cuda_state = 0; return 0; }
    if (g_cuda.cuCtxCreate(&g_cuda.ctx, 0, dev) != 0) { g_cuda_state = 0; return 0; }

    /* Compile the kernel to PTX. */
    const char* opts[] = { "--gpu-architecture=compute_60",
                           "--std=c++11" };
    void* prog = NULL;
    if (g_cuda.nvrtcCreateProgram(&prog, VAYU_CUDA_KERNEL_SRC,
                                  "vayu_q16.cu", 0, NULL, NULL) != 0) {
        g_cuda_state = 0; return 0;
    }
    if (g_cuda.nvrtcCompileProgram(prog, 2, opts) != 0) {
        g_cuda.nvrtcDestroyProgram(&prog);
        g_cuda_state = 0; return 0;
    }
    size_t ptx_size = 0;
    if (g_cuda.nvrtcGetPTXSize(prog, &ptx_size) != 0) {
        g_cuda.nvrtcDestroyProgram(&prog);
        g_cuda_state = 0; return 0;
    }
    char* ptx = (char*)malloc(ptx_size);
    if (g_cuda.nvrtcGetPTX(prog, ptx) != 0) {
        free(ptx); g_cuda.nvrtcDestroyProgram(&prog);
        g_cuda_state = 0; return 0;
    }
    g_cuda.nvrtcDestroyProgram(&prog);

    VayuCUmodule mod = NULL;
    if (g_cuda.cuModuleLoadData(&mod, ptx) != 0) {
        free(ptx); g_cuda_state = 0; return 0;
    }
    free(ptx);

    if (g_cuda.cuModuleGetFunction(&g_cuda.matmul_kernel, mod,
                                   "vayu_q16_matmul") != 0) {
        g_cuda_state = 0; return 0;
    }

    g_cuda.inited = 1;
    g_cuda_state = 1;
    return 1;
}

int64_t vayu_cuda_available(void) {
    return vayu_cuda_init() ? 1 : 0;
}
int64_t vayu_cuda_device_count(void) {
    if (!vayu_cuda_init()) return 0;
    return (int64_t)g_cuda.device_count;
}

int64_t vayu_cuda_matmul(int64_t a_h, int64_t b_h) {
    VayuTensor* a = (VayuTensor*)a_h;
    VayuTensor* b = (VayuTensor*)b_h;
    if (!a || !b || a->ndim != 2 || b->ndim != 2) return 0;
    if (a->shape[1] != b->shape[0]) return 0;
    if (!vayu_cuda_init()) return 0;

    int M = (int)a->shape[0];
    int K = (int)a->shape[1];
    int N = (int)b->shape[1];

    size_t aBytes = (size_t)M * (size_t)K * sizeof(int64_t);
    size_t bBytes = (size_t)K * (size_t)N * sizeof(int64_t);
    size_t cBytes = (size_t)M * (size_t)N * sizeof(int64_t);

    void *dA = NULL, *dB = NULL, *dC = NULL;
    if (g_cuda.cuMemAlloc(&dA, aBytes) != 0) return 0;
    if (g_cuda.cuMemAlloc(&dB, bBytes) != 0) { g_cuda.cuMemFree(dA); return 0; }
    if (g_cuda.cuMemAlloc(&dC, cBytes) != 0) {
        g_cuda.cuMemFree(dA); g_cuda.cuMemFree(dB); return 0;
    }

    g_cuda.cuMemcpyHtoD(dA, a->data, aBytes);
    g_cuda.cuMemcpyHtoD(dB, b->data, bBytes);

    unsigned bx = 16, by = 16;
    unsigned gx = (unsigned)((N + bx - 1) / bx);
    unsigned gy = (unsigned)((M + by - 1) / by);

    void* args[] = { (void*)&dA, (void*)&dB, (void*)&dC,
                     (void*)&M, (void*)&K, (void*)&N };
    if (g_cuda.cuLaunchKernel(g_cuda.matmul_kernel,
                              gx, gy, 1, bx, by, 1, 0, NULL, args, NULL) != 0) {
        g_cuda.cuMemFree(dA); g_cuda.cuMemFree(dB); g_cuda.cuMemFree(dC);
        return 0;
    }
    g_cuda.cuCtxSynchronize();

    int64_t sh[2] = { M, N };
    int64_t oh = vayu_tensor_new_from_shape(sh, 2);
    VayuTensor* o = (VayuTensor*)oh;
    if (o) g_cuda.cuMemcpyDtoH(o->data, dC, cBytes);

    g_cuda.cuMemFree(dA); g_cuda.cuMemFree(dB); g_cuda.cuMemFree(dC);
    return oh;
}

void vayu_cuda_shutdown(void) {
    if (g_cuda_state == 1 && g_cuda.ctx && g_cuda.cuCtxDestroy) {
        g_cuda.cuCtxDestroy(g_cuda.ctx);
    }
    if (g_cuda.lib_nvrtc) FreeLibrary((HMODULE)g_cuda.lib_nvrtc);
    if (g_cuda.lib_nvcuda) FreeLibrary((HMODULE)g_cuda.lib_nvcuda);
    g_cuda.lib_nvrtc = NULL;
    g_cuda.lib_nvcuda = NULL;
    g_cuda_state = -1;
}

#else /* !_WIN32 */

int64_t vayu_cuda_available(void) { return 0; }
int64_t vayu_cuda_device_count(void) { return 0; }
int64_t vayu_cuda_matmul(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
void    vayu_cuda_shutdown(void) {}
int64_t vayu_dml_available(void) { return 0; }
int64_t vayu_dml_matmul(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
void    vayu_dml_shutdown(void) {}
void    vayu_cuda_shutdown(void) {}

#endif

/* ===========================================================================
 * Phase 22.4b - D3D11 compute-shader matmul.  Runs on any Windows GPU.
 * No CUDA toolkit required.  "DML" here means "GPU compute via the OS
 * graphics stack", not the DirectML.dll ML API (that needs D3D12 and
 * ships only as a redistributable).
 * ========================================================================= */

#ifdef _WIN32

typedef struct {
    ID3D11Device*        device;
    ID3D11DeviceContext* ctx;
    ID3D11ComputeShader* cs;
    ID3D11Buffer*        cb;
    int                  ok;
} VayuDML;

static VayuDML g_dml;
static int     g_dml_state = -1;

static const char* VAYU_DML_HLSL =
    "StructuredBuffer<float> A : register(t0);\n"
    "StructuredBuffer<float> B : register(t1);\n"
    "RWStructuredBuffer<float> C : register(u0);\n"
    "cbuffer P : register(b0) { int M; int K; int N; int pad; };\n"
    "[numthreads(16,16,1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
    "  int i = (int)id.y; int j = (int)id.x;\n"
    "  if (i >= M || j >= N) return;\n"
    "  float s = 0.0f;\n"
    "  for (int k = 0; k < K; ++k)\n"
    "    s += A[i*K + k] * B[k*N + j];\n"
    "  C[i*N + j] = s;\n"
    "}\n";

static int vayu_dml_init(void) {
    if (g_dml_state >= 0) return g_dml_state;
    memset(&g_dml, 0, sizeof(g_dml));
    const int dbg = (getenv("VAYU_DML_DEBUG") != NULL);
    if (dbg) fprintf(stderr, "[dml] init starting\n");

    D3D_FEATURE_LEVEL want[3] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                   want, 3, D3D11_SDK_VERSION,
                                   &g_dml.device, &got, &g_dml.ctx);
    if (FAILED(hr) || !g_dml.device) {
        if (dbg) fprintf(stderr, "[dml] D3D11CreateDevice failed hr=0x%08lx\n",
                         (unsigned long)hr);
        g_dml_state = 0; return 0;
    }
    if (dbg) fprintf(stderr, "[dml] device created, feature level=0x%x\n",
                     (unsigned)got);

    ID3DBlob* blob = NULL; ID3DBlob* err = NULL;
    hr = D3DCompile(VAYU_DML_HLSL, strlen(VAYU_DML_HLSL),
                    "vayu_matmul", NULL, NULL, "CSMain", "cs_5_0", 0, 0,
                    &blob, &err);
    if (dbg && err) {
        const char* msg = (const char*)err->lpVtbl->GetBufferPointer(err);
        fprintf(stderr, "[dml] HLSL compile error:\n%s\n", msg);
    }
    if (err) err->lpVtbl->Release(err);
    if (FAILED(hr) || !blob) {
        if (dbg) fprintf(stderr, "[dml] HLSL compile failed hr=0x%08lx\n",
                         (unsigned long)hr);
        g_dml.ctx->lpVtbl->Release(g_dml.ctx);
        g_dml.device->lpVtbl->Release(g_dml.device);
        g_dml_state = 0; return 0;
    }
    if (dbg) fprintf(stderr, "[dml] HLSL compiled, %zu bytes\n",
                     (size_t)blob->lpVtbl->GetBufferSize(blob));
    hr = g_dml.device->lpVtbl->CreateComputeShader(g_dml.device,
        blob->lpVtbl->GetBufferPointer(blob),
        blob->lpVtbl->GetBufferSize(blob),
        NULL, &g_dml.cs);
    blob->lpVtbl->Release(blob);
    if (FAILED(hr) || !g_dml.cs) {
        if (dbg) fprintf(stderr, "[dml] CreateComputeShader failed hr=0x%08lx\n",
                         (unsigned long)hr);
        g_dml.ctx->lpVtbl->Release(g_dml.ctx);
        g_dml.device->lpVtbl->Release(g_dml.device);
        g_dml_state = 0; return 0;
    }

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dml.device->lpVtbl->CreateBuffer(g_dml.device, &cbd, NULL, &g_dml.cb);
    if (FAILED(hr)) {
        g_dml.cs->lpVtbl->Release(g_dml.cs);
        g_dml.ctx->lpVtbl->Release(g_dml.ctx);
        g_dml.device->lpVtbl->Release(g_dml.device);
        g_dml_state = 0; return 0;
    }

    g_dml.ok = 1;
    g_dml_state = 1;
    return 1;
}

int64_t vayu_dml_available(void) { return vayu_dml_init() ? 1 : 0; }

int64_t vayu_dml_matmul(int64_t a_h, int64_t b_h) {
    VayuTensor* a = (VayuTensor*)a_h;
    VayuTensor* b = (VayuTensor*)b_h;
    if (!a || !b || a->ndim != 2 || b->ndim != 2) return 0;
    if (a->shape[1] != b->shape[0]) return 0;
    if (!vayu_dml_init()) return 0;

    int M = (int)a->shape[0], K = (int)a->shape[1], N = (int)b->shape[1];

    /* Convert Q16.16 -> float.  The GPU kernel works in float FMA;
       float is what every Windows GPU of the last 15 years accelerates. */
    float* aF = (float*)malloc(sizeof(float) * (size_t)(M * K));
    float* bF = (float*)malloc(sizeof(float) * (size_t)(K * N));
    float* cF = (float*)malloc(sizeof(float) * (size_t)(M * N));
    if (!aF || !bF || !cF) { free(aF); free(bF); free(cF); return 0; }

    for (int64_t i = 0; i < (int64_t)M * K; ++i)
        aF[i] = (float)((double)a->data[i] / 65536.0);
    for (int64_t i = 0; i < (int64_t)K * N; ++i)
        bF[i] = (float)((double)b->data[i] / 65536.0);

    size_t aB = (size_t)M * K * 4;
    size_t bB = (size_t)K * N * 4;
    size_t cB = (size_t)M * N * 4;

    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer* bufA = NULL; ID3D11Buffer* bufB = NULL; ID3D11Buffer* bufC = NULL;
    ID3D11ShaderResourceView* srvA = NULL; ID3D11ShaderResourceView* srvB = NULL;
    ID3D11UnorderedAccessView* uavC = NULL;
    ID3D11Buffer* stage = NULL;

    memset(&bd, 0, sizeof(bd));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;

    bd.ByteWidth = (UINT)aB;
    memset(&sd, 0, sizeof(sd)); sd.pSysMem = aF;
    if (FAILED(g_dml.device->lpVtbl->CreateBuffer(g_dml.device, &bd, &sd, &bufA))) goto done;
    bd.ByteWidth = (UINT)bB;
    sd.pSysMem = bF;
    if (FAILED(g_dml.device->lpVtbl->CreateBuffer(g_dml.device, &bd, &sd, &bufB))) goto done;

    memset(&bd, 0, sizeof(bd));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = (UINT)cB;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    if (FAILED(g_dml.device->lpVtbl->CreateBuffer(g_dml.device, &bd, NULL, &bufC))) goto done;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
    memset(&srvd, 0, sizeof(srvd));
    srvd.Format = DXGI_FORMAT_UNKNOWN;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvd.Buffer.FirstElement = 0;
    srvd.Buffer.NumElements = (UINT)(M * K);
    if (FAILED(g_dml.device->lpVtbl->CreateShaderResourceView(
        g_dml.device, (ID3D11Resource*)bufA, &srvd, &srvA))) goto done;
    srvd.Buffer.NumElements = (UINT)(K * N);
    if (FAILED(g_dml.device->lpVtbl->CreateShaderResourceView(
        g_dml.device, (ID3D11Resource*)bufB, &srvd, &srvB))) goto done;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd;
    memset(&uavd, 0, sizeof(uavd));
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.FirstElement = 0;
    uavd.Buffer.NumElements = (UINT)(M * N);
    if (FAILED(g_dml.device->lpVtbl->CreateUnorderedAccessView(
        g_dml.device, (ID3D11Resource*)bufC, &uavd, &uavC))) goto done;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_dml.ctx->lpVtbl->Map(g_dml.ctx, (ID3D11Resource*)g_dml.cb,
                                      0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) goto done;
    ((int*)ms.pData)[0] = M;
    ((int*)ms.pData)[1] = K;
    ((int*)ms.pData)[2] = N;
    ((int*)ms.pData)[3] = 0;
    g_dml.ctx->lpVtbl->Unmap(g_dml.ctx, (ID3D11Resource*)g_dml.cb, 0);

    ID3D11ShaderResourceView* srvs[2] = { srvA, srvB };
    ID3D11UnorderedAccessView* uavs[1] = { uavC };
    ID3D11Buffer* cbs[1] = { g_dml.cb };
    g_dml.ctx->lpVtbl->CSSetShader(g_dml.ctx, g_dml.cs, NULL, 0);
    g_dml.ctx->lpVtbl->CSSetShaderResources(g_dml.ctx, 0, 2, srvs);
    g_dml.ctx->lpVtbl->CSSetUnorderedAccessViews(g_dml.ctx, 0, 1, uavs, NULL);
    g_dml.ctx->lpVtbl->CSSetConstantBuffers(g_dml.ctx, 0, 1, cbs);
    g_dml.ctx->lpVtbl->Dispatch(g_dml.ctx, (UINT)((M + 15) / 16),
                                         (UINT)((N + 15) / 16), 1);

    ID3D11UnorderedAccessView* nullUav[1] = { NULL };
    ID3D11ShaderResourceView* nullSrv[2] = { NULL, NULL };
    g_dml.ctx->lpVtbl->CSSetUnorderedAccessViews(g_dml.ctx, 0, 1, nullUav, NULL);
    g_dml.ctx->lpVtbl->CSSetShaderResources(g_dml.ctx, 0, 2, nullSrv);

    memset(&bd, 0, sizeof(bd));
    bd.Usage = D3D11_USAGE_STAGING;
    bd.ByteWidth = (UINT)cB;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dml.device->lpVtbl->CreateBuffer(g_dml.device, &bd, NULL, &stage))) goto done;
    g_dml.ctx->lpVtbl->CopyResource(g_dml.ctx, (ID3D11Resource*)stage,
                                             (ID3D11Resource*)bufC);
    if (FAILED(g_dml.ctx->lpVtbl->Map(g_dml.ctx, (ID3D11Resource*)stage,
                                      0, D3D11_MAP_READ, 0, &ms))) goto done;

    memcpy(cF, ms.pData, cB);
    g_dml.ctx->lpVtbl->Unmap(g_dml.ctx, (ID3D11Resource*)stage, 0);

    {
        int64_t sh[2] = { M, N };
        int64_t oh = vayu_tensor_new_from_shape(sh, 2);
        VayuTensor* o = (VayuTensor*)oh;
        if (o) {
            for (int64_t i = 0; i < (int64_t)M * N; ++i) {
                double dv = (double)cF[i];
                o->data[i] = (int64_t)(dv * 65536.0 + (dv < 0.0 ? -0.5 : 0.5));
            }
        }
        free(aF); free(bF); free(cF);
        if (stage) stage->lpVtbl->Release(stage);
        if (uavC)  uavC->lpVtbl->Release(uavC);
        if (srvA)  srvA->lpVtbl->Release(srvA);
        if (srvB)  srvB->lpVtbl->Release(srvB);
        if (bufA)  bufA->lpVtbl->Release(bufA);
        if (bufB)  bufB->lpVtbl->Release(bufB);
        if (bufC)  bufC->lpVtbl->Release(bufC);
        return oh;
    }

done:
    free(aF); free(bF); free(cF);
    if (stage) stage->lpVtbl->Release(stage);
    if (uavC)  uavC->lpVtbl->Release(uavC);
    if (srvA)  srvA->lpVtbl->Release(srvA);
    if (srvB)  srvB->lpVtbl->Release(srvB);
    if (bufA)  bufA->lpVtbl->Release(bufA);
    if (bufB)  bufB->lpVtbl->Release(bufB);
    if (bufC)  bufC->lpVtbl->Release(bufC);
    return 0;
}

void vayu_dml_shutdown(void) {
    if (g_dml_state == 1) {
        if (g_dml.cb)     g_dml.cb->lpVtbl->Release(g_dml.cb);
        if (g_dml.cs)     g_dml.cs->lpVtbl->Release(g_dml.cs);
        if (g_dml.ctx)    g_dml.ctx->lpVtbl->Release(g_dml.ctx);
        if (g_dml.device) g_dml.device->lpVtbl->Release(g_dml.device);
    }
    memset(&g_dml, 0, sizeof(g_dml));
    g_dml_state = -1;
}

#else
int64_t vayu_dml_available(void) { return 0; }
int64_t vayu_dml_matmul(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
void    vayu_dml_shutdown(void) {}
#endif

int64_t vayu_nn_run_str(int64_t text_sp, int64_t input_h) {
    VayuStr* text = (VayuStr*)text_sp;
    if (!text) return 0;

    VayuNNModel model;
    memset(&model, 0, sizeof(model));

    size_t n = (size_t)text->len;
    char* buf = (char*)malloc(n + 1);
    memcpy(buf, text->data, n);
    buf[n] = 0;

    const char* p = buf;
    char last_dst[64] = {0};

    while (*p) {
        vayu_nn_skip_ws(&p);
        if (*p == '#' || *p == '\n' || *p == 0) {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        char kw[32];
        vayu_nn_read_word(&p, kw, sizeof(kw));

        if (strcmp(kw, "input") == 0) {
            char nm[64];
            vayu_nn_read_word(&p, nm, sizeof(nm));
            int k = 0;
            while (nm[k] && k < 63) { model.input_name[k] = nm[k]; k++; }
            model.input_name[k] = 0;
            if (input_h) vayu_nn_bind(&model, nm, (VayuTensor*)input_h);
        }
        else if (strcmp(kw, "tensor") == 0) {
            char nm[64];
            vayu_nn_read_word(&p, nm, sizeof(nm));
            int rows = (int)vayu_nn_read_int(&p);
            int cols = (int)vayu_nn_read_int(&p);
            if (rows > 0 && cols > 0) {
                int64_t sh[2] = { rows, cols };
                int64_t th = vayu_tensor_new_from_shape(sh, 2);
                VayuTensor* t = (VayuTensor*)th;
                if (t) {
                    for (int64_t i = 0; i < t->numel; ++i)
                        t->data[i] = vayu_nn_read_int(&p);
                }
                vayu_nn_bind(&model, nm, t);
            }
        }
        else if (strcmp(kw, "op") == 0) {
            char op[32], t1[64], t2[64], t3[64];
            vayu_nn_read_word(&p, op, sizeof(op));
            vayu_nn_read_word(&p, t1, sizeof(t1));
            vayu_nn_read_word(&p, t2, sizeof(t2));
            vayu_nn_read_word(&p, t3, sizeof(t3));
            char dst[64], a[64], b[64];
            if (t3[0] != 0) {
                /* binary: op OP A B DST */
                int k;
                for (k = 0; t1[k] && k < 63; k++) a[k] = t1[k];
                a[k] = 0;
                for (k = 0; t2[k] && k < 63; k++) b[k] = t2[k];
                b[k] = 0;
                for (k = 0; t3[k] && k < 63; k++) dst[k] = t3[k];
                dst[k] = 0;
            } else {
                /* unary:  op OP A DST */
                int k;
                for (k = 0; t1[k] && k < 63; k++) a[k] = t1[k];
                a[k] = 0;
                b[0] = 0;
                for (k = 0; t2[k] && k < 63; k++) dst[k] = t2[k];
                dst[k] = 0;
            }

            int ia = vayu_nn_lookup(&model, a);
            int ib = (b[0] != 0) ? vayu_nn_lookup(&model, b) : -1;
            if (ia >= 0) {
                int64_t r = 0;
                int64_t ta = (int64_t)model.binds[ia].t;
                int64_t tb = (ib >= 0) ? (int64_t)model.binds[ib].t : 0;
                if (strcmp(op, "matmul") == 0 || strcmp(op, "gemm") == 0) {
                    if (ib >= 0) r = vayu_tensor_matmul(ta, tb);
                } else if (strcmp(op, "add") == 0) {
                    if (ib >= 0) r = vayu_tensor_add(ta, tb);
                } else if (strcmp(op, "sub") == 0) {
                    if (ib >= 0) r = vayu_tensor_sub(ta, tb);
                } else if (strcmp(op, "mul") == 0) {
                    if (ib >= 0) r = vayu_tensor_mul(ta, tb);
                } else if (strcmp(op, "div") == 0) {
                    if (ib >= 0) r = vayu_tensor_div(ta, tb);
                } else if (strcmp(op, "relu") == 0) {
                    r = vayu_tensor_relu(ta);
                } else if (strcmp(op, "sigmoid") == 0) {
                    r = vayu_tensor_sigmoid(ta);
                } else if (strcmp(op, "tanh") == 0) {
                    r = vayu_tensor_tanh(ta);
                }
                if (r) {
                    vayu_nn_bind(&model, dst, (VayuTensor*)r);
                    int k = 0;
                    while (dst[k] && k < 63) { last_dst[k] = dst[k]; k++; }
                    last_dst[k] = 0;
                }
            }
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (model.input_name[0] && input_h) {
        vayu_nn_bind(&model, model.input_name, (VayuTensor*)input_h);
    }

    int64_t out = 0;
    if (last_dst[0]) {
        int idx = vayu_nn_lookup(&model, last_dst);
        if (idx >= 0) out = (int64_t)model.binds[idx].t;
    }
    free(buf);
    return out;
}

void vayu_tensor_backward(int64_t h) {
    VayuTensor* t = (VayuTensor*)h;
    if (!t) return;
    if (t->grad) t->grad = 0;
    t->grad = vayu_tensor_ones_like(h);

    g_in_backward = 1;
    for (int i = g_tape_len - 1; i >= 0; --i) {
        VayuTapeEntry* e = &g_tape[i];
        VayuTensor* out = (VayuTensor*)e->out;
        if (!out || !out->grad) continue;
        int64_t g_h = out->grad;
        VayuTensor* g = (VayuTensor*)g_h;

        switch (e->op) {
            case VAYU_OP_ADD: {
                int64_t dA = vayu_tensor_copy(g_h);
                vayu_grad_accum(e->a, dA);
                int64_t dB = vayu_tensor_copy(g_h);
                vayu_grad_accum(e->b, dB);
                break;
            }
            case VAYU_OP_SUB: {
                int64_t dA = vayu_tensor_copy(g_h);
                vayu_grad_accum(e->a, dA);
                int64_t dB = vayu_tensor_neg(g_h);
                vayu_grad_accum(e->b, dB);
                break;
            }
            case VAYU_OP_MUL: {
                int64_t dA = vayu_tensor_mul(g_h, e->b);
                vayu_grad_accum(e->a, dA);
                int64_t dB = vayu_tensor_mul(g_h, e->a);
                vayu_grad_accum(e->b, dB);
                break;
            }
            case VAYU_OP_DIV: {
                /* y = a/b;  dA = g/b;  dB = -g*a/b^2 */
                int64_t dA = vayu_tensor_div(g_h, e->b);
                vayu_grad_accum(e->a, dA);
                int64_t bb = vayu_tensor_mul(e->b, e->b);
                int64_t ga = vayu_tensor_mul(g_h, e->a);
                int64_t q = vayu_tensor_div(ga, bb);
                int64_t dB = vayu_tensor_neg(q);
                vayu_grad_accum(e->b, dB);
                break;
            }
            case VAYU_OP_MATMUL: {
                int64_t bT = vayu_tensor_transpose(e->b);
                int64_t aT = vayu_tensor_transpose(e->a);
                int64_t dA = vayu_tensor_matmul(g_h, bT);
                vayu_grad_accum(e->a, dA);
                int64_t dB = vayu_tensor_matmul(aT, g_h);
                vayu_grad_accum(e->b, dB);
                break;
            }
            case VAYU_OP_SUM: {
                VayuTensor* a = (VayuTensor*)e->a;
                int64_t r = vayu_tensor_new_from_shape(a->shape, a->ndim);
                VayuTensor* o = (VayuTensor*)r;
                /* data is now `double`, not Q16.16.  Casting to int64
                   would truncate any gradient smaller than 1.0 to zero. */
                double gv = g->data[0];
                for (int64_t k = 0; k < a->numel; ++k) o->data[k] = gv;
                vayu_grad_accum(e->a, r);
                break;
            }
            case VAYU_OP_MUL_SCALAR: {
                int64_t dA = vayu_tensor_mul_scalar(g_h, e->scalar);
                vayu_grad_accum(e->a, dA);
                break;
            }
            case VAYU_OP_ADD_SCALAR: {
                int64_t dA = vayu_tensor_copy(g_h);
                vayu_grad_accum(e->a, dA);
                break;
            }
            case VAYU_OP_RELU:
            case VAYU_OP_SIGMOID:
            case VAYU_OP_TANH:
            case VAYU_OP_EXP:
            case VAYU_OP_LOG: {
                VayuTensor* x = (VayuTensor*)e->a;
                VayuTensor* y = out;
                int64_t r = vayu_tensor_new_from_shape(x->shape, x->ndim);
                VayuTensor* o = (VayuTensor*)r;
                for (int64_t k = 0; k < x->numel; ++k) {
                    double xi = x->data[k], yi = y->data[k], gi = g->data[k];
                    double d = 0.0;
                    switch (e->op) {
                        case VAYU_OP_RELU:    d = (xi > 0.0) ? 1.0 : 0.0;       break;
                        case VAYU_OP_SIGMOID: d = yi * (1.0 - yi);              break;
                        case VAYU_OP_TANH:    d = 1.0 - yi * yi;                break;
                        case VAYU_OP_EXP:     d = yi;                           break;
                        case VAYU_OP_LOG:     d = (xi > 0.0) ? (1.0 / xi) : 0.0; break;
                    }
                    o->data[k] = gi * d;
                }
                vayu_grad_accum(e->a, r);
                break;
            }
            case VAYU_OP_TRANSPOSE: {
                int64_t dA = vayu_tensor_transpose(g_h);
                vayu_grad_accum(e->a, dA);
                break;
            }
        }
    }
    g_in_backward = 0;
    (void)vayu_unary_local;
    (void)vayu_tensor_ones_shape;
}

/* Blit a VayuTexture onto a VayuCanvas.  Unlike gui.draw_bitmap, which
   takes a decoded file bitmap, this wraps the raw level-0 pixel buffer in
   a transient GpBitmap and draws it.  Used by post-processing pipelines
   where the source is a framebuffer snapshot, not a loaded image. */
void vayu_raster_tex_present(int64_t texh, int64_t canvas_h,
                             int64_t x, int64_t y) {
    VayuTexture* t = (VayuTexture*)texh;
    VayuCanvas*  c = (VayuCanvas*)canvas_h;
    if (!t || t->level_count < 1 || !c || !c->g) return;
    VayuTexLevel* L = &t->levels[0];
    if (!L->pixels) return;
    GpBitmap* bmp = NULL;
    GdipCreateBitmapFromScan0((INT)L->w, (INT)L->h, (INT)(L->w * 4),
                              PixelFormat32bppARGB,
                              (BYTE*)L->pixels, &bmp);
    if (!bmp) return;
    GdipDrawImageI(c->g, (GpImage*)bmp, (INT)x, (INT)y);
    GdipDisposeImage((GpImage*)bmp);
}

// ---- child controls ----

int64_t vayu_gui_create_input(int64_t parent, int64_t x, int64_t y, int64_t w, int64_t h) {
    HWND e = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
        (int)x, (int)y, (int)w, (int)h,
        (HWND)parent, NULL, GetModuleHandleA(NULL), NULL);
    if (e) g_edit_old_proc = (WNDPROC)SetWindowLongPtrA(e, GWLP_WNDPROC, (LONG_PTR)vayu_gui_edit_proc);
    return (int64_t)e;
}
int64_t vayu_gui_input_get(int64_t e) {
    HWND h = (HWND)e; if (!h) return (int64_t)vayu_mkstr("", 0);
    int n = GetWindowTextLengthA(h);
    if (n <= 0) return (int64_t)vayu_mkstr("", 0);
    char* buf = (char*)malloc((size_t)n+1);
    GetWindowTextA(h, buf, n+1);
    VayuStr* s = vayu_mkstr(buf, n); free(buf);
    return (int64_t)s;
}
void vayu_gui_input_set(int64_t e, VayuStr* s) { HWND h=(HWND)e; if(h) SetWindowTextA(h, s->data); }
void vayu_gui_input_focus(int64_t e) { HWND h=(HWND)e; if(h) SetFocus(h); }

int64_t vayu_gui_create_checkbox(int64_t parent, int64_t x, int64_t y, int64_t w, int64_t h, VayuStr* label) {
    HWND cb = CreateWindowExA(0, "BUTTON", label->data,
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        (int)x, (int)y, (int)w, (int)h,
        (HWND)parent, NULL, GetModuleHandleA(NULL), NULL);
    return (int64_t)cb;
}
int64_t vayu_gui_checkbox_get(int64_t e) {
    HWND h = (HWND)e; if (!h) return 0;
    return SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
}
void vayu_gui_checkbox_set(int64_t e, int64_t v) {
    HWND h = (HWND)e; if (!h) return;
    SendMessageA(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
}

int64_t vayu_gui_create_slider(int64_t parent, int64_t x, int64_t y, int64_t w, int64_t h) {
    HWND tb = CreateWindowExA(0, TRACKBAR_CLASS, "",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        (int)x, (int)y, (int)w, (int)h,
        (HWND)parent, NULL, GetModuleHandleA(NULL), NULL);
    if (!tb) return 0;
    SendMessageA(tb, TBM_SETRANGE,    TRUE, (LPARAM)MAKELONG(0, 100));
    SendMessageA(tb, TBM_SETPOS,      TRUE, (LPARAM)0);
    SendMessageA(tb, TBM_SETPAGESIZE, 0,    (LPARAM)10);
    SendMessageA(tb, TBM_SETLINESIZE, 0,    (LPARAM)1);
    return (int64_t)tb;
}
int64_t vayu_gui_slider_get(int64_t e) {
    HWND h = (HWND)e; if (!h) return 0;
    return (int64_t)SendMessageA(h, TBM_GETPOS, 0, 0);
}
void vayu_gui_slider_set(int64_t e, int64_t v) {
    HWND h = (HWND)e; if (!h) return;
    SendMessageA(h, TBM_SETPOS, TRUE, (LPARAM)v);
}
void vayu_gui_slider_set_range(int64_t e, int64_t lo, int64_t hi) {
    HWND h = (HWND)e; if (!h) return;
    SendMessageA(h, TBM_SETRANGE, TRUE, (LPARAM)MAKELONG((short)lo, (short)hi));
}

int64_t vayu_gui_create_listbox(int64_t parent, int64_t x, int64_t y, int64_t w, int64_t h) {
    HWND lb = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP,
        (int)x, (int)y, (int)w, (int)h,
        (HWND)parent, NULL, GetModuleHandleA(NULL), NULL);
    return (int64_t)lb;
}
void vayu_gui_listbox_add(int64_t e, VayuStr* s) {
    HWND h=(HWND)e; if(h) SendMessageA(h, LB_ADDSTRING, 0, (LPARAM)s->data);
}
int64_t vayu_gui_listbox_count(int64_t e) {
    HWND h=(HWND)e; return h ? (int64_t)SendMessageA(h, LB_GETCOUNT, 0, 0) : 0;
}
int64_t vayu_gui_listbox_index(int64_t e) {
    HWND h=(HWND)e; return h ? (int64_t)SendMessageA(h, LB_GETCURSEL, 0, 0) : -1;
}
int64_t vayu_gui_listbox_get(int64_t e, int64_t idx) {
    HWND h=(HWND)e; if (!h) return (int64_t)vayu_mkstr("", 0);
    LRESULT n = SendMessageA(h, LB_GETTEXTLEN, (WPARAM)idx, 0);
    if (n <= 0) return (int64_t)vayu_mkstr("", 0);
    char* buf = (char*)malloc((size_t)n+1);
    SendMessageA(h, LB_GETTEXT, (WPARAM)idx, (LPARAM)buf);
    VayuStr* s = vayu_mkstr(buf, n); free(buf);
    return (int64_t)s;
}
void vayu_gui_listbox_clear(int64_t e) {
    HWND h=(HWND)e; if(h) SendMessageA(h, LB_RESETCONTENT, 0, 0);
}
void vayu_gui_listbox_set_index(int64_t e, int64_t idx) {
    HWND h=(HWND)e; if(h) SendMessageA(h, LB_SETCURSEL, (WPARAM)idx, 0);
}

#else  // !_WIN32

int64_t vayu_gui_init(void) { return 0; }
int64_t vayu_gui_create_window(VayuStr* t, int64_t w, int64_t h) { (void)t;(void)w;(void)h; return 0; }
void vayu_gui_show_window(int64_t h) { (void)h; }
void vayu_gui_invalidate(void) {}
void vayu_gui_set_title(int64_t h, VayuStr* t) { (void)h;(void)t; }
void vayu_gui_set_size(int64_t h, int64_t w, int64_t k) { (void)h;(void)w;(void)k; }
void vayu_gui_set_callback(int64_t fp) { (void)fp; }
void vayu_gui_set_timer(int64_t ms) { (void)ms; }
void vayu_gui_clear_timer(void) {}
int64_t vayu_gui_mouse_x(void) { return 0; }
int64_t vayu_gui_mouse_y(void) { return 0; }
int64_t vayu_gui_last_key(void) { return -1; }
int64_t vayu_gui_wheel_delta(void) { return 0; }
int64_t vayu_gui_window_w(void) { return 0; }
int64_t vayu_gui_window_h(void) { return 0; }
int64_t vayu_gui_run(int64_t h) { (void)h; return 0; }
void vayu_gui_close(int64_t h) { (void)h; }
void vayu_gui_quit(void) {}
void vayu_gui_clear(int64_t c) { (void)c; }
void vayu_gui_fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void vayu_gui_outline_rect(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void vayu_gui_line(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t c) { (void)x1;(void)y1;(void)x2;(void)y2;(void)c; }
void vayu_gui_text(int64_t sp, int64_t x, int64_t y, int64_t c) { (void)sp;(void)x;(void)y;(void)c; }
int64_t vayu_gui_rect_hit(int64_t x, int64_t y, int64_t w, int64_t h, int64_t px, int64_t py) { (void)x;(void)y;(void)w;(void)h;(void)px;(void)py; return 0; }
int64_t vayu_gui_create_input(int64_t p, int64_t x, int64_t y, int64_t w, int64_t h) { (void)p;(void)x;(void)y;(void)w;(void)h; return 0; }
int64_t vayu_gui_input_get(int64_t e) { (void)e; return 0; }
void vayu_gui_input_set(int64_t e, VayuStr* s) { (void)e;(void)s; }
void vayu_gui_input_focus(int64_t e) { (void)e; }
int64_t vayu_gui_create_checkbox(int64_t p, int64_t x, int64_t y, int64_t w, int64_t h, VayuStr* l) { (void)p;(void)x;(void)y;(void)w;(void)h;(void)l; return 0; }
int64_t vayu_gui_checkbox_get(int64_t e) { (void)e; return 0; }
void vayu_gui_checkbox_set(int64_t e, int64_t v) { (void)e;(void)v; }
int64_t vayu_gui_create_slider(int64_t p, int64_t x, int64_t y, int64_t w, int64_t h) { (void)p;(void)x;(void)y;(void)w;(void)h; return 0; }
int64_t vayu_gui_slider_get(int64_t e) { (void)e; return 0; }
void vayu_gui_slider_set(int64_t e, int64_t v) { (void)e;(void)v; }
void vayu_gui_slider_set_range(int64_t e, int64_t lo, int64_t hi) { (void)e;(void)lo;(void)hi; }
int64_t vayu_gui_create_listbox(int64_t p, int64_t x, int64_t y, int64_t w, int64_t h) { (void)p;(void)x;(void)y;(void)w;(void)h; return 0; }
void vayu_gui_listbox_add(int64_t e, VayuStr* s) { (void)e;(void)s; }
int64_t vayu_gui_listbox_count(int64_t e) { (void)e; return 0; }
int64_t vayu_gui_listbox_index(int64_t e) { (void)e; return -1; }
int64_t vayu_gui_listbox_get(int64_t e, int64_t i) { (void)e;(void)i; return 0; }
void vayu_gui_listbox_clear(int64_t e) { (void)e; }
void vayu_gui_listbox_set_index(int64_t e, int64_t i) { (void)e;(void)i; }

/* ---- Phase 20.1 / 20.2 stubs (non-Windows) ---- */
int64_t vayu_gui_canvas_new(int64_t w, int64_t h) { (void)w;(void)h; return 0; }
int64_t vayu_gui_canvas_from_window(void) { return 0; }
void vayu_gui_canvas_free(int64_t h) { (void)h; }
void vayu_gui_canvas_clear(int64_t h, int64_t argb) { (void)h;(void)argb; }
void vayu_gui_canvas_fill_rect(int64_t h, int64_t x, int64_t y, int64_t w, int64_t k, int64_t argb) { (void)h;(void)x;(void)y;(void)w;(void)k;(void)argb; }
void vayu_gui_canvas_outline_rect(int64_t h, int64_t x, int64_t y,
                                  int64_t w, int64_t k,
                                  int64_t argb, int64_t thickness) {
    VayuCanvas* c = (VayuCanvas*)h;
    if (!c || !c->g) return;
    REAL saved = c->line_width;
    c->line_width = (REAL)thickness;
    GpPen* p = vayu_canvas_make_pen(c, (ARGB)argb);
    GdipDrawRectangleI(c->g, p, (INT)x, (INT)y, (INT)w, (INT)k);
    c->line_width = saved;
}
void vayu_gui_blit_canvas(int64_t src, int64_t x, int64_t y) { (void)src;(void)x;(void)y; }
void vayu_gui_circle(int64_t h, int64_t cx, int64_t cy, int64_t r, int64_t argb) { (void)h;(void)cx;(void)cy;(void)r;(void)argb; }
void vayu_gui_circle_outline(int64_t h, int64_t cx, int64_t cy, int64_t r, int64_t argb) { (void)h;(void)cx;(void)cy;(void)r;(void)argb; }
void vayu_gui_ellipse(int64_t h, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t argb) { (void)h;(void)cx;(void)cy;(void)rx;(void)ry;(void)argb; }
void vayu_gui_ellipse_outline(int64_t h, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t argb) { (void)h;(void)cx;(void)cy;(void)rx;(void)ry;(void)argb; }
void vayu_gui_rounded_rect(int64_t h, int64_t x, int64_t y, int64_t w, int64_t k, int64_t r, int64_t argb) { (void)h;(void)x;(void)y;(void)w;(void)k;(void)r;(void)argb; }
void vayu_gui_rounded_rect_outline(int64_t h, int64_t x, int64_t y, int64_t w, int64_t k, int64_t r, int64_t argb) { (void)h;(void)x;(void)y;(void)w;(void)k;(void)r;(void)argb; }
void vayu_gui_arc(int64_t h, int64_t cx, int64_t cy, int64_t r, int64_t sd, int64_t sw, int64_t argb) { (void)h;(void)cx;(void)cy;(void)r;(void)sd;(void)sw;(void)argb; }
void vayu_gui_polygon(int64_t h, int64_t pts, int64_t argb) { (void)h;(void)pts;(void)argb; }
void vayu_gui_polygon_outline(int64_t h, int64_t pts, int64_t argb) { (void)h;(void)pts;(void)argb; }
void vayu_gui_line_width(int64_t h, int64_t px) { (void)h;(void)px; }
void vayu_gui_line_cap(int64_t h, int64_t m) { (void)h;(void)m; }
void vayu_gui_line_join(int64_t h, int64_t m) { (void)h;(void)m; }
int64_t vayu_gui_font_new(int64_t n, int64_t s, int64_t w, int64_t i) { (void)n;(void)s;(void)w;(void)i; return 0; }
void vayu_gui_font_free(int64_t h) { (void)h; }
int64_t vayu_gui_font_default(void) { return 0; }
void vayu_gui_text_ex(int64_t c, int64_t f, int64_t s, int64_t x, int64_t y, int64_t a) { (void)c;(void)f;(void)s;(void)x;(void)y;(void)a; }
int64_t vayu_gui_bitmap_load(int64_t p) { (void)p; return 0; }
void vayu_gui_bitmap_free(int64_t h) { (void)h; }
int64_t vayu_gui_bitmap_width(int64_t h) { (void)h; return 0; }
int64_t vayu_gui_bitmap_height(int64_t h) { (void)h; return 0; }
void vayu_gui_draw_bitmap(int64_t c, int64_t b, int64_t x, int64_t y) { (void)c;(void)b;(void)x;(void)y; }
void vayu_gui_draw_bitmap_scaled(int64_t c, int64_t b, int64_t x, int64_t y, int64_t w, int64_t k) { (void)c;(void)b;(void)x;(void)y;(void)w;(void)k; }
void vayu_gui_draw_bitmap_part(int64_t c, int64_t b, int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t dx, int64_t dy) { (void)c;(void)b;(void)sx;(void)sy;(void)sw;(void)sh;(void)dx;(void)dy; }
void vayu_gui_draw_bitmap_alpha(int64_t c, int64_t b, int64_t x, int64_t y, int64_t a) { (void)c;(void)b;(void)x;(void)y;(void)a; }
int64_t vayu_gui_canvas_save_png(int64_t c, int64_t p) { (void)c;(void)p; return 0; }
int64_t vayu_gui_bitmap_from_pixels(int64_t w, int64_t h, int64_t l) { (void)w;(void)h;(void)l; return 0; }
void vayu_gui_push_transform(int64_t h) { (void)h; }
void vayu_gui_pop_transform(int64_t h) { (void)h; }
void vayu_gui_reset_transform(int64_t h) { (void)h; }
void vayu_gui_translate(int64_t h, int64_t dx, int64_t dy) { (void)h;(void)dx;(void)dy; }
void vayu_gui_rotate(int64_t h, int64_t d) { (void)h;(void)d; }
void vayu_gui_rotate_at(int64_t h, int64_t d, int64_t cx, int64_t cy) { (void)h;(void)d;(void)cx;(void)cy; }
void vayu_gui_scale(int64_t h, int64_t sx, int64_t sy) { (void)h;(void)sx;(void)sy; }
void vayu_gui_clip_rect(int64_t h, int64_t x, int64_t y, int64_t w, int64_t k) { (void)h;(void)x;(void)y;(void)w;(void)k; }
void vayu_gui_clip_reset(int64_t h) { (void)h; }
void vayu_gui_fill_mode(int64_t h, int64_t m) { (void)h;(void)m; }
void vayu_gui_compositing_mode(int64_t h, int64_t m) { (void)h;(void)m; }
void vayu_gui_text_align(int64_t h, int64_t m) { (void)h;(void)m; }
int64_t vayu_gui_text_width(int64_t h, int64_t f, int64_t s) { (void)h;(void)f;(void)s; return 0; }
int64_t vayu_gui_text_height(int64_t h, int64_t f, int64_t s) { (void)h;(void)f;(void)s; return 0; }
int64_t vayu_gui_font_height(int64_t f) { (void)f; return 0; }
int64_t vayu_gui_font_line_spacing(int64_t f) { (void)f; return 0; }

/* ---- Phase 21.1 raster stubs (non-Windows) ---- */
int64_t vayu_raster_fb_new(int64_t w, int64_t h) { (void)w;(void)h; return 0; }
void vayu_raster_fb_free(int64_t h) { (void)h; }
int64_t vayu_raster_fb_width(int64_t h) { (void)h; return 0; }
int64_t vayu_raster_fb_height(int64_t h) { (void)h; return 0; }
void vayu_raster_fb_clear(int64_t h, int64_t c) { (void)h;(void)c; }
void vayu_raster_fb_set(int64_t h, int64_t x, int64_t y, int64_t c) { (void)h;(void)x;(void)y;(void)c; }
int64_t vayu_raster_fb_get(int64_t h, int64_t x, int64_t y) { (void)h;(void)x;(void)y; return 0; }
void vayu_raster_fb_present(int64_t h, int64_t c, int64_t x, int64_t y) { (void)h;(void)c;(void)x;(void)y; }
void vayu_raster_draw_line(int64_t h, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) { (void)h;(void)a;(void)b;(void)c;(void)d;(void)e; }
void vayu_raster_draw_tri(int64_t h, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g) { (void)h;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; }
void vayu_raster_fb_enable_depth(int64_t h) { (void)h; }
void vayu_raster_fb_disable_depth(int64_t h) { (void)h; }
void vayu_raster_fb_clear_depth(int64_t h) { (void)h; }
int64_t vayu_raster_mat_new(void) { return 0; }
void vayu_raster_mat_free(int64_t h) { (void)h; }
void vayu_raster_mat_identity(int64_t h) { (void)h; }
void vayu_raster_mat_mul(int64_t d, int64_t a, int64_t b) { (void)d;(void)a;(void)b; }
void vayu_raster_mat_translate(int64_t h, int64_t x, int64_t y, int64_t z) { (void)h;(void)x;(void)y;(void)z; }
void vayu_raster_mat_rotate_x(int64_t h, int64_t d) { (void)h;(void)d; }
void vayu_raster_mat_rotate_y(int64_t h, int64_t d) { (void)h;(void)d; }
void vayu_raster_mat_rotate_z(int64_t h, int64_t d) { (void)h;(void)d; }
void vayu_raster_mat_perspective(int64_t h, int64_t f, int64_t a, int64_t n, int64_t ff) { (void)h;(void)f;(void)a;(void)n;(void)ff; }
void vayu_raster_mat_look_at(int64_t h, int64_t ex, int64_t ey, int64_t ez, int64_t tx, int64_t ty, int64_t tz, int64_t ux, int64_t uy, int64_t uz) { (void)h;(void)ex;(void)ey;(void)ez;(void)tx;(void)ty;(void)tz;(void)ux;(void)uy;(void)uz; }
int64_t vayu_raster_mesh_new(void) { return 0; }
void vayu_raster_mesh_free(int64_t h) { (void)h; }
void vayu_raster_mesh_clear(int64_t h) { (void)h; }
void vayu_raster_mesh_add_vert(int64_t h, int64_t x, int64_t y, int64_t z, int64_t u, int64_t v) { (void)h;(void)x;(void)y;(void)z;(void)u;(void)v; }
void vayu_raster_mesh_add_tri(int64_t h, int64_t i0, int64_t i1, int64_t i2) { (void)h;(void)i0;(void)i1;(void)i2; }
void vayu_raster_draw_mesh(int64_t fb, int64_t m, int64_t mt, int64_t t, int64_t tn) { (void)fb;(void)m;(void)mt;(void)t;(void)tn; }
int64_t vayu_raster_tex_new(int64_t w, int64_t h) { (void)w;(void)h; return 0; }
void vayu_raster_tex_free(int64_t h) { (void)h; }
int64_t vayu_raster_tex_width(int64_t h) { (void)h; return 0; }
int64_t vayu_raster_tex_height(int64_t h) { (void)h; return 0; }
void vayu_raster_tex_set(int64_t h, int64_t x, int64_t y, int64_t c) { (void)h;(void)x;(void)y;(void)c; }
void vayu_raster_tex_set_filter(int64_t h, int64_t m) { (void)h;(void)m; }
void vayu_raster_tex_gen_mipmaps(int64_t h) { (void)h; }
void vayu_raster_mesh_add_vert_lit(int64_t h, int64_t x, int64_t y, int64_t z, int64_t nx, int64_t ny, int64_t nz, int64_t u, int64_t v) { (void)h;(void)x;(void)y;(void)z;(void)nx;(void)ny;(void)nz;(void)u;(void)v; }
void vayu_raster_mesh_set_normal(int64_t h, int64_t i, int64_t nx, int64_t ny, int64_t nz) { (void)h;(void)i;(void)nx;(void)ny;(void)nz; }
void vayu_raster_set_ambient(int64_t a) { (void)a; }
void vayu_raster_light_clear(void) {}
void vayu_raster_light_set(int64_t i, int64_t k, int64_t x, int64_t y, int64_t z, int64_t c, int64_t q) { (void)i;(void)k;(void)x;(void)y;(void)z;(void)c;(void)q; }
void vayu_raster_draw_mesh_lit(int64_t fb, int64_t m, int64_t mvp, int64_t mod, int64_t tx, int64_t sm, int64_t tn, int64_t ex, int64_t ey, int64_t ez) { (void)fb;(void)m;(void)mvp;(void)mod;(void)tx;(void)sm;(void)tn;(void)ex;(void)ey;(void)ez; }
void vayu_raster_post_gamma(int64_t t, int64_t g) { (void)t;(void)g; }
void vayu_raster_post_invert(int64_t t) { (void)t; }
void vayu_raster_post_invert(int64_t t) { (void)t; }
void vayu_raster_post_tint(int64_t t, int64_t c) { (void)t;(void)c; }
void vayu_raster_post_brightness(int64_t t, int64_t d) { (void)t;(void)d; }
void vayu_raster_post_threshold(int64_t t, int64_t th) { (void)t;(void)th; }
int64_t vayu_raster_tex_from_fb(int64_t fb) { (void)fb; return 0; }
void vayu_raster_tex_present(int64_t t, int64_t c, int64_t x, int64_t y) { (void)t;(void)c;(void)x;(void)y; }

/* ---- Phase 22.0 + 22.1 tensor stubs (non-Windows) ---- */
int64_t vayu_tensor_new(int64_t s) { (void)s; return 0; }
int64_t vayu_tensor_zeros(int64_t s) { (void)s; return 0; }
int64_t vayu_tensor_ones(int64_t s) { (void)s; return 0; }
int64_t vayu_tensor_from_int_list(int64_t s, int64_t v) { (void)s;(void)v; return 0; }
int64_t vayu_tensor_copy(int64_t h) { (void)h; return 0; }
void    vayu_tensor_free(int64_t h) { (void)h; }
void    vayu_tensor_fill(int64_t h, int64_t v) { (void)h;(void)v; }
int64_t vayu_tensor_ndim(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_shape(int64_t h, int64_t i) { (void)h;(void)i; return 0; }
int64_t vayu_tensor_numel_h(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_get_flat(int64_t h, int64_t i) { (void)h;(void)i; return 0; }
void    vayu_tensor_set_flat(int64_t h, int64_t i, int64_t v) { (void)h;(void)i;(void)v; }
int64_t vayu_tensor_add(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
int64_t vayu_tensor_sub(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
int64_t vayu_tensor_mul(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
int64_t vayu_tensor_div(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
int64_t vayu_tensor_add_scalar(int64_t a, int64_t v) { (void)a;(void)v; return 0; }
int64_t vayu_tensor_mul_scalar(int64_t a, int64_t v) { (void)a;(void)v; return 0; }
int64_t vayu_tensor_matmul(int64_t a, int64_t b) { (void)a;(void)b; return 0; }
int64_t vayu_tensor_sum(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_max(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_argmax(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_reshape(int64_t h, int64_t s) { (void)h;(void)s; return 0; }
int64_t vayu_tensor_transpose(int64_t h) { (void)h; return 0; }
void    vayu_tensor_print(int64_t h, int64_t nm) { (void)h;(void)nm; }
void    vayu_tensor_requires_grad(int64_t h, int64_t f) { (void)h;(void)f; }
void    vayu_tensor_zero_grad(int64_t h) { (void)h; }
int64_t vayu_tensor_grad(int64_t h) { (void)h; return 0; }
void    vayu_tensor_backward(int64_t h) { (void)h; }
void    vayu_tensor_tape_clear(void) {}
int64_t vayu_tensor_tape_size(void) { return 0; }
int64_t vayu_tensor_relu(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_sigmoid(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_tanh(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_exp(int64_t h) { (void)h; return 0; }
int64_t vayu_tensor_log(int64_t h) { (void)h; return 0; }
void    vayu_tensor_copy_into(int64_t d, int64_t s) { (void)d;(void)s; }
int64_t vayu_nn_run_str(int64_t t, int64_t i) { (void)t;(void)i; return 0; }
int64_t vayu_onnx_load(int64_t p) { (void)p; return 0; }
void    vayu_onnx_free(int64_t h) { (void)h; }
int64_t vayu_onnx_run(int64_t m, int64_t i) { (void)m;(void)i; return 0; }
int64_t vayu_onnx_input_name(int64_t h) { (void)h; return 0; }
int64_t vayu_onnx_output_name(int64_t h) { (void)h; return 0; }
void    vayu_onnx_debug(int64_t h) { (void)h; }

#endif
// ---- try/except (thread-local for generator workers) ----

#define VAYU_MAX_TRY 64

#ifdef _MSC_VER
#  define VAYU_THREAD_LOCAL __declspec(thread)
#else
#  define VAYU_THREAD_LOCAL __thread
#endif

static VAYU_THREAD_LOCAL jmp_buf  g_jmpBufs[VAYU_MAX_TRY];
static VAYU_THREAD_LOCAL int      g_trySp = 0;
static VAYU_THREAD_LOCAL VayuExc* g_excValue = NULL;

int vayu_try_push(void) {
    if (g_trySp >= VAYU_MAX_TRY) {
        fprintf(stderr, "vayu: try stack overflow\n"); exit(1);
    }
    return g_trySp++;
}
void* vayu_try_buf(int id) { return &g_jmpBufs[id]; }
void vayu_try_pop(void) { if (g_trySp > 0) g_trySp--; }

VayuStr* vayu_get_exc_type(void) {
    return g_excValue ? g_excValue->typeName : NULL;
}
VayuExc* vayu_get_exc_value(void) { return g_excValue; }

VayuExc* vayu_mkexc(VayuStr* typeName, VayuStr* msg) {
    VayuExc* e = (VayuExc*)malloc(sizeof(VayuExc));
    e->typeName = typeName;
    e->message  = msg;
    return e;
}

void vayu_raise(VayuExc* e) {
    if (g_trySp == 0) {
        fwrite(e->typeName->data, 1, (size_t)e->typeName->len, stderr);
        fprintf(stderr, ": ");
        fwrite(e->message->data, 1, (size_t)e->message->len, stderr);
        fprintf(stderr, "\n");
        exit(1);
    }
    g_excValue = e;
    longjmp(g_jmpBufs[g_trySp - 1], 1);
}

void vayu_raise_str(VayuStr* typeName, VayuStr* msg) {
    VayuExc* e = vayu_mkexc(typeName, msg);
    vayu_raise(e);
}

void vayu_reraise(void) {
    if (g_trySp == 0) { fprintf(stderr, "vayu: uncaught\n"); exit(1); }
    longjmp(g_jmpBufs[g_trySp - 1], 1);
}

// ---- fs ----
static char* vayu_fs_cstr(VayuStr* s) {
    char* buf = (char*)malloc((size_t)s->len + 1);
    memcpy(buf, s->data, (size_t)s->len);
    buf[s->len] = 0;
    return buf;
}
static int64_t vayu_fs_stat_mode(const char* p) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p, &st) != 0) return -1;
    return (int64_t)st.st_mode;
#else
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (int64_t)st.st_mode;
#endif
}
int64_t vayu_fs_exists(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t mode = vayu_fs_stat_mode(p);
    free(p);
    return mode < 0 ? 0 : 1;
}
int64_t vayu_fs_is_file(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t mode = vayu_fs_stat_mode(p);
    free(p);
    if (mode < 0) return 0;
#ifdef _WIN32
    return (mode & _S_IFREG) ? 1 : 0;
#else
    return S_ISREG(mode) ? 1 : 0;
#endif
}
int64_t vayu_fs_is_dir(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t mode = vayu_fs_stat_mode(p);
    free(p);
    if (mode < 0) return 0;
#ifdef _WIN32
    return (mode & _S_IFDIR) ? 1 : 0;
#else
    return S_ISDIR(mode) ? 1 : 0;
#endif
}
int64_t vayu_fs_size(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p, &st) != 0) { free(p); return 0; }
#else
    struct stat st;
    if (stat(p, &st) != 0) { free(p); return 0; }
#endif
    int64_t sz = (int64_t)st.st_size;
    free(p);
    return sz;
}
VayuStr* vayu_fs_cwd(void) {
    char buf[4096];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#endif
    return vayu_mkstr_c(buf);
}
VayuStr* vayu_fs_abs(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
#ifdef _WIN32
    char full[4096];
    if (_fullpath(full, p, sizeof(full)) == NULL) { free(p); return vayu_mkstr("", 0); }
    free(p);
    return vayu_mkstr_c(full);
#else
    if (p[0] == '/') { VayuStr* r = vayu_mkstr_c(p); free(p); return r; }
    char* cwd_buf = getcwd(NULL, 0);
    if (!cwd_buf) { free(p); return vayu_mkstr("", 0); }
    size_t clen = strlen(cwd_buf);
    size_t plen = strlen(p);
    char* full = (char*)malloc(clen + 1 + plen + 1);
    memcpy(full, cwd_buf, clen);
    full[clen] = '/';
    memcpy(full + clen + 1, p, plen);
    full[clen + 1 + plen] = 0;
    free(cwd_buf); free(p);
    VayuStr* r = vayu_mkstr_c(full);
    free(full);
    return r;
#endif
}
VayuStr* vayu_fs_join(VayuStr* a, VayuStr* b) {
    if (a->len == 0) return vayu_mkstr(b->data, b->len);
    char last = a->data[a->len - 1];
    if (last == '/' || last == '\\') return vayu_str_concat(a, b);
#ifdef _WIN32
    return vayu_str_concat(vayu_str_concat(a, vayu_mkstr_c("\\")), b);
#else
    return vayu_str_concat(vayu_str_concat(a, vayu_mkstr_c("/")), b);
#endif
}
VayuStr* vayu_fs_extension(VayuStr* path) {
    int64_t dot = -1;
    int64_t i = path->len - 1;
    while (i >= 0) {
        char c = path->data[i];
        if (c == '.') { dot = i; break; }
        if (c == '/' || c == '\\') break;
        i = i - 1;
    }
    if (dot < 0) return vayu_mkstr("", 0);
    return vayu_mkstr(path->data + dot + 1, path->len - dot - 1);
}
VayuStr* vayu_fs_basename(VayuStr* path) {
    int64_t slash = -1;
    for (int64_t i = 0; i < path->len; ++i) {
        char c = path->data[i];
        if (c == '/' || c == '\\') slash = i;
    }
    return vayu_mkstr(path->data + slash + 1, path->len - slash - 1);
}
VayuStr* vayu_fs_dirname(VayuStr* path) {
    int64_t slash = -1;
    for (int64_t i = 0; i < path->len; ++i) {
        char c = path->data[i];
        if (c == '/' || c == '\\') slash = i;
    }
    if (slash < 0) return vayu_mkstr_c(".");
    if (slash == 0) return vayu_mkstr(path->data, 1);
    return vayu_mkstr(path->data, slash);
}
void vayu_fs_remove(VayuStr* path) { char* p = vayu_fs_cstr(path); remove(p); free(p); }
void vayu_fs_rename(VayuStr* a, VayuStr* b) {
    char* pa = vayu_fs_cstr(a);
    char* pb = vayu_fs_cstr(b);
    rename(pa, pb);
    free(pa); free(pb);
}
void vayu_fs_mkdir(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t n = (int64_t)strlen(p);
    for (int64_t i = 0; i < n; ++i) {
        if (p[i] == '/' || p[i] == '\\') {
            char save = p[i];
            p[i] = 0;
            if (p[0] != 0) {
#ifdef _WIN32
                _mkdir(p);
#else
                mkdir(p, 0755);
#endif
            }
            p[i] = save;
        }
    }
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
    free(p);
}
void vayu_fs_rmdir(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    char cmd[8192];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "cmd /c rmdir /s /q \"%s\" 2>nul", p);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" 2>/dev/null", p);
#endif
    system(cmd);
    free(p);
}
VayuList* vayu_fs_read_dir(VayuStr* path) {
    VayuList* lst = vayu_list_new();
    char* p = vayu_fs_cstr(path);
    DIR* d = opendir(p);
    if (!d) { free(p); return lst; }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* n = ent->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        vayu_list_push(lst, (int64_t)vayu_mkstr_c(n));
    }
    closedir(d);
    free(p);
    return lst;
}

// ---- time ----
int64_t vayu_time_now(void) { return (int64_t)time(NULL); }
int64_t vayu_time_now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER ui;
    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return (int64_t)((ui.QuadPart / 10000ULL) - 11644473600000LL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}
void vayu_time_sleep(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}
VayuStr* vayu_time_format(int64_t unix_secs, VayuStr* fmt) {
    char fmtbuf[256];
    int64_t n = fmt->len < 255 ? fmt->len : 255;
    memcpy(fmtbuf, fmt->data, (size_t)n);
    fmtbuf[n] = 0;
    time_t t = (time_t)unix_secs;
    struct tm tmv;
#ifdef _WIN32
    if (localtime_s(&tmv, &t) != 0) return vayu_mkstr("", 0);
#else
    if (localtime_r(&t, &tmv) == NULL) return vayu_mkstr("", 0);
#endif
    char out[512];
    size_t len = strftime(out, sizeof(out), fmtbuf, &tmv);
    return vayu_mkstr(out, (int64_t)len);
}

// ---- json ----
typedef struct VayuJsonValue {
    int32_t tag;
    int32_t pad;
    int64_t data;
} VayuJsonValue;
typedef struct { const char* s; int64_t n; int64_t i; } JsonParser;
static VayuJsonValue* jp_new(int32_t tag, int64_t data) {
    VayuJsonValue* v = (VayuJsonValue*)malloc(sizeof(VayuJsonValue));
    v->tag = tag; v->pad = 0; v->data = data;
    return v;
}
static void jp_skip_ws(JsonParser* p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}
static VayuJsonValue* jp_parse_value(JsonParser* p);
static VayuJsonValue* jp_parse_string(JsonParser* p) {
    p->i++;
    size_t cap = 32, len = 0;
    char* buf = (char*)malloc(cap);
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') { p->i++; break; }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) break;
            char e = p->s[p->i++];
            if (e == 'u') {
                if (p->i + 4 > p->n) continue;
                unsigned int cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = p->s[p->i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                }
                if (len + 5 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                if (cp < 0x80) buf[len++] = (char)cp;
                else if (cp < 0x800) {
                    buf[len++] = (char)(0xC0 | (cp >> 6));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    buf[len++] = (char)(0xE0 | (cp >> 12));
                    buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                }
                continue;
            }
            char out;
            switch (e) {
                case 'n': out = '\n'; break;
                case 't': out = '\t'; break;
                case 'r': out = '\r'; break;
                case 'b': out = '\b'; break;
                case 'f': out = '\f'; break;
                case '"': out = '"'; break;
                case '\\': out = '\\'; break;
                case '/': out = '/'; break;
                default:  out = e; break;
            }
            if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            buf[len++] = out;
            continue;
        }
        if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = (char)c;
        p->i++;
    }
    VayuStr* s = vayu_mkstr(buf, (int64_t)len);
    free(buf);
    return jp_new(3, (int64_t)s);
}
static VayuJsonValue* jp_parse_number(JsonParser* p) {
    int neg = 0;
    if (p->i < p->n && p->s[p->i] == '-') { neg = 1; p->i++; }
    int64_t n = 0;
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        n = n * 10 + (p->s[p->i] - '0');
        p->i++;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (neg) n = -n;
    return jp_new(2, n);
}
static VayuJsonValue* jp_parse_array(JsonParser* p) {
    p->i++;
    VayuList* lst = vayu_list_new();
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') {
        p->i++;
        return jp_new(4, (int64_t)lst);
    }
    while (p->i < p->n) {
        jp_skip_ws(p);
        VayuJsonValue* v = jp_parse_value(p);
        vayu_list_push(lst, (int64_t)v);
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
        break;
    }
    return jp_new(4, (int64_t)lst);
}
static VayuJsonValue* jp_parse_object(JsonParser* p) {
    p->i++;
    VayuMap* m = vayu_map_new();
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') {
        p->i++;
        return jp_new(5, (int64_t)m);
    }
    while (p->i < p->n) {
        jp_skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != '"') break;
        VayuJsonValue* k = jp_parse_string(p);
        VayuStr* key = (VayuStr*)k->data;
        free(k);
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ':') p->i++;
        jp_skip_ws(p);
        VayuJsonValue* v = jp_parse_value(p);
        vayu_map_put(m, key, (int64_t)v);
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
        break;
    }
    return jp_new(5, (int64_t)m);
}
static VayuJsonValue* jp_parse_value(JsonParser* p) {
    jp_skip_ws(p);
    if (p->i >= p->n) return jp_new(0, 0);
    char c = p->s[p->i];
    if (c == 'n' && p->i + 4 <= p->n && strncmp(p->s + p->i, "null", 4) == 0) {
        p->i += 4; return jp_new(0, 0);
    }
    if (c == 't' && p->i + 4 <= p->n && strncmp(p->s + p->i, "true", 4) == 0) {
        p->i += 4; return jp_new(1, 1);
    }
    if (c == 'f' && p->i + 5 <= p->n && strncmp(p->s + p->i, "false", 5) == 0) {
        p->i += 5; return jp_new(1, 0);
    }
    if (c == '"') return jp_parse_string(p);
    if (c == '[') return jp_parse_array(p);
    if (c == '{') return jp_parse_object(p);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p);
    return jp_new(0, 0);
}
VayuJsonValue* vayu_json_parse(VayuStr* s) {
    JsonParser p;
    p.s = s->data;
    p.n = s->len;
    p.i = 0;
    return jp_parse_value(&p);
}
static void vayu_json_stringify_to(VayuJsonValue* v, VayuList* chunks) {
    if (!v) { vayu_list_push(chunks, (int64_t)vayu_mkstr_c("null")); return; }
    if (v->tag == 0) { vayu_list_push(chunks, (int64_t)vayu_mkstr_c("null")); return; }
    if (v->tag == 1) {
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c(v->data ? "true" : "false"));
        return;
    }
    if (v->tag == 2) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld", (long long)v->data);
        vayu_list_push(chunks, (int64_t)vayu_mkstr(buf, n));
        return;
    }
    if (v->tag == 3) {
        VayuStr* s = (VayuStr*)v->data;
        size_t cap = (size_t)s->len + 16, len = 0;
        char* buf = (char*)malloc(cap);
        buf[len++] = '"';
        for (int64_t i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->data[i];
            const char* esc = NULL;
            switch (c) {
                case '"':  esc = "\\\""; break;
                case '\\': esc = "\\\\"; break;
                case '\n': esc = "\\n";  break;
                case '\r': esc = "\\r";  break;
                case '\t': esc = "\\t";  break;
                case '\b': esc = "\\b";  break;
                case '\f': esc = "\\f";  break;
            }
            if (esc) {
                while (len + 4 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                buf[len++] = esc[0]; buf[len++] = esc[1];
            } else if (c < 0x20) {
                while (len + 8 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                len += (size_t)snprintf(buf + len, cap - len, "\\u%04x", c);
            } else {
                while (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                buf[len++] = (char)c;
            }
        }
        buf[len++] = '"';
        VayuStr* out = vayu_mkstr(buf, (int64_t)len);
        free(buf);
        vayu_list_push(chunks, (int64_t)out);
        return;
    }
    if (v->tag == 4) {
        VayuList* lst = (VayuList*)v->data;
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("["));
        for (int64_t i = 0; i < lst->len; i++) {
            if (i) vayu_list_push(chunks, (int64_t)vayu_mkstr_c(","));
            vayu_json_stringify_to((VayuJsonValue*)lst->items[i], chunks);
        }
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("]"));
        return;
    }
    if (v->tag == 5) {
        VayuMap* m = (VayuMap*)v->data;
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("{"));
        int64_t printed = 0;
        for (int64_t i = 0; i < m->cap; i++) {
            if (!m->entries[i].used) continue;
            if (printed) vayu_list_push(chunks, (int64_t)vayu_mkstr_c(","));
            VayuJsonValue tmp;
            tmp.tag = 3; tmp.pad = 0; tmp.data = (int64_t)m->entries[i].key;
            vayu_json_stringify_to(&tmp, chunks);
            vayu_list_push(chunks, (int64_t)vayu_mkstr_c(":"));
            vayu_json_stringify_to((VayuJsonValue*)m->entries[i].value, chunks);
            printed++;
        }
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("}"));
        return;
    }
    vayu_list_push(chunks, (int64_t)vayu_mkstr_c("null"));
}
VayuStr* vayu_json_stringify(VayuJsonValue* v) {
    VayuList* chunks = vayu_list_new();
    vayu_json_stringify_to(v, chunks);
    int64_t total = 0;
    for (int64_t i = 0; i < chunks->len; i++)
        total += ((VayuStr*)chunks->items[i])->len;
    VayuStr* out = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)total + 1);
    out->len = total;
    int64_t op = 0;
    for (int64_t i = 0; i < chunks->len; i++) {
        VayuStr* c = (VayuStr*)chunks->items[i];
        memcpy(out->data + op, c->data, (size_t)c->len);
        op += c->len;
    }
    out->data[total] = 0;
    return out;
}
VayuJsonValue* vayu_json_get(VayuJsonValue* v, VayuStr* key) {
    if (!v || v->tag != 5) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.get: value is not an object"));
    }
    VayuMapEntry* e = map_find((VayuMap*)v->data, key);
    if (!e) vayu_raise_str(vayu_mkstr_c("KeyError"), key);
    return (VayuJsonValue*)e->value;
}
VayuJsonValue* vayu_json_index(VayuJsonValue* v, int64_t i) {
    if (!v || v->tag != 4) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.index: value is not an array"));
    }
    VayuList* lst = (VayuList*)v->data;
    if (i < 0) i += lst->len;
    if (i < 0 || i >= lst->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("json.index: index out of range"));
    }
    return (VayuJsonValue*)lst->items[i];
}
int64_t vayu_json_as_int(VayuJsonValue* v) {
    if (!v || v->tag != 2) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.as_int: value is not a number"));
    }
    return v->data;
}
VayuStr* vayu_json_as_str(VayuJsonValue* v) {
    if (!v || v->tag != 3) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.as_str: value is not a string"));
    }
    return (VayuStr*)v->data;
}
int64_t vayu_json_as_bool(VayuJsonValue* v) {
    if (!v || v->tag != 1) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.as_bool: value is not a boolean"));
    }
    return v->data;
}
int64_t vayu_json_len(VayuJsonValue* v) {
    if (!v) return 0;
    if (v->tag == 4) return ((VayuList*)v->data)->len;
    if (v->tag == 5) return ((VayuMap*)v->data)->len;
    return 0;
}
int64_t vayu_json_has(VayuJsonValue* v, VayuStr* key) {
    if (!v || v->tag != 5) return 0;
    return map_find((VayuMap*)v->data, key) != NULL ? 1 : 0;
}
VayuStr* vayu_json_type(VayuJsonValue* v) {
    if (!v) return vayu_mkstr_c("null");
    if (v->tag == 0) return vayu_mkstr_c("null");
    if (v->tag == 1) return vayu_mkstr_c("bool");
    if (v->tag == 2) return vayu_mkstr_c("int");
    if (v->tag == 3) return vayu_mkstr_c("str");
    if (v->tag == 4) return vayu_mkstr_c("list");
    if (v->tag == 5) return vayu_mkstr_c("map");
    return vayu_mkstr_c("?");
}
int64_t vayu_json_is_null(VayuJsonValue* v) { return (!v || v->tag == 0) ? 1 : 0; }
int64_t vayu_json_is_int (VayuJsonValue* v) { return (v && v->tag == 2) ? 1 : 0; }
int64_t vayu_json_is_str (VayuJsonValue* v) { return (v && v->tag == 3) ? 1 : 0; }
int64_t vayu_json_is_bool(VayuJsonValue* v) { return (v && v->tag == 1) ? 1 : 0; }
int64_t vayu_json_is_list(VayuJsonValue* v) { return (v && v->tag == 4) ? 1 : 0; }
int64_t vayu_json_is_map (VayuJsonValue* v) { return (v && v->tag == 5) ? 1 : 0; }
VayuList* vayu_json_keys(VayuJsonValue* v) {
    if (!v || v->tag != 5) return vayu_list_new();
    VayuMap* m = (VayuMap*)v->data;
    VayuList* out = vayu_list_new();
    for (int64_t i = 0; i < m->cap; i++) {
        if (!m->entries[i].used) continue;
        vayu_list_push(out, (int64_t)m->entries[i].key);
    }
    return out;
}
VayuJsonValue* vayu_json_make_null(void)          { return jp_new(0, 0); }
VayuJsonValue* vayu_json_make_bool(int64_t b)     { return jp_new(1, b ? 1 : 0); }
VayuJsonValue* vayu_json_make_int (int64_t n)     { return jp_new(2, n); }
VayuJsonValue* vayu_json_make_str (VayuStr* s)    { return jp_new(3, (int64_t)s); }

// ---- regex ----
typedef struct {
    const char* pat; int64_t plen;
    const char* txt; int64_t tlen;
} VayuRx;
static int vayu_rx_match_one(VayuRx* r, int64_t pi, int64_t ti,
                             int64_t* adv_pi, int64_t* adv_ti) {
    if (pi >= r->plen) return 0;
    if (ti >= r->tlen) return 0;
    char pc = r->pat[pi];
    char tc = r->txt[ti];
    if (pc == '.') { *adv_pi = pi + 1; *adv_ti = ti + 1; return 1; }
    if (pc == '\\') {
        if (pi + 1 >= r->plen) return 0;
        char e = r->pat[pi + 1];
        int ok = 0;
        if (e == 'd') ok = (tc >= '0' && tc <= '9');
        else if (e == 'D') ok = !(tc >= '0' && tc <= '9');
        else if (e == 'w') ok = (tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z')
                              || (tc >= '0' && tc <= '9') || tc == '_';
        else if (e == 'W') ok = !((tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z')
                              || (tc >= '0' && tc <= '9') || tc == '_');
        else if (e == 's') ok = (tc == ' ' || tc == '\t' || tc == '\n' ||
                                 tc == '\r' || tc == '\f' || tc == '\v');
        else if (e == 'S') ok = !(tc == ' ' || tc == '\t' || tc == '\n' ||
                                  tc == '\r' || tc == '\f' || tc == '\v');
        else ok = (tc == e);
        if (!ok) return 0;
        *adv_pi = pi + 2; *adv_ti = ti + 1;
        return 1;
    }
    if (pc == '[') {
        int64_t i = pi + 1;
        int neg = 0;
        if (i < r->plen && r->pat[i] == '^') { neg = 1; i++; }
        int found = 0;
        while (i < r->plen && r->pat[i] != ']') {
            if (r->pat[i] == '\\' && i + 1 < r->plen) {
                char e = r->pat[i + 1];
                int hit = 0;
                if (e == 'd') hit = (tc >= '0' && tc <= '9');
                else if (e == 'w') hit = (tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z')
                                      || (tc >= '0' && tc <= '9') || tc == '_';
                else if (e == 's') hit = (tc == ' ' || tc == '\t' || tc == '\n' || tc == '\r');
                else hit = (tc == e);
                if (hit) found = 1;
                i += 2;
                continue;
            }
            if (i + 2 < r->plen && r->pat[i + 1] == '-' && r->pat[i + 2] != ']') {
                char lo = r->pat[i];
                char hi = r->pat[i + 2];
                if (tc >= lo && tc <= hi) found = 1;
                i += 3;
                continue;
            }
            if (tc == r->pat[i]) found = 1;
            i++;
        }
        if (i >= r->plen || r->pat[i] != ']') return 0;
        if (neg) found = !found;
        if (!found) return 0;
        *adv_pi = i + 1; *adv_ti = ti + 1;
        return 1;
    }
    if (tc != pc) return 0;
    *adv_pi = pi + 1; *adv_ti = ti + 1;
    return 1;
}
static int64_t vayu_rx_atom_end(VayuRx* r, int64_t pi) {
    char c = r->pat[pi];
    if (c == '\\') return pi + 2;
    if (c == '[') {
        int64_t i = pi + 1;
        if (i < r->plen && r->pat[i] == '^') i++;
        while (i < r->plen && r->pat[i] != ']') {
            if (r->pat[i] == '\\') i += 2; else i++;
        }
        return i + 1;
    }
    return pi + 1;
}
static int vayu_rx_here(VayuRx* r, int64_t pi, int64_t ti, int64_t* out_end) {
    if (pi >= r->plen) { *out_end = ti; return 1; }
    char c = r->pat[pi];
    if (c == '^') {
        if (ti != 0) return 0;
        return vayu_rx_here(r, pi + 1, ti, out_end);
    }
    if (c == '$') {
        if (ti != r->tlen) return 0;
        return vayu_rx_here(r, pi + 1, ti, out_end);
    }
    int64_t atom_end = vayu_rx_atom_end(r, pi);
    char q = (atom_end < r->plen) ? r->pat[atom_end] : 0;
    if (q == '*' || q == '+') {
        int64_t cur_pi = atom_end + 1;
        int64_t positions[2048];
        int count = 0;
        positions[count++] = ti;
        int64_t tp = ti;
        while (count < 2048) {
            int64_t np = 0, nt = 0;
            if (!vayu_rx_match_one(r, pi, tp, &np, &nt)) break;
            tp = nt;
            positions[count++] = tp;
        }
        int min_matches = (q == '+') ? 1 : 0;
        for (int k = count - 1; k >= min_matches; k--) {
            if (vayu_rx_here(r, cur_pi, positions[k], out_end)) return 1;
        }
        return 0;
    }
    if (q == '?') {
        int64_t cur_pi = atom_end + 1;
        int64_t np = 0, nt = 0;
        if (vayu_rx_match_one(r, pi, ti, &np, &nt)) {
            if (vayu_rx_here(r, cur_pi, nt, out_end)) return 1;
        }
        return vayu_rx_here(r, cur_pi, ti, out_end);
    }
    int64_t np = 0, nt = 0;
    if (!vayu_rx_match_one(r, pi, ti, &np, &nt)) return 0;
    return vayu_rx_here(r, np, nt, out_end);
}
static int vayu_rx_find(VayuRx* r, int64_t from, int64_t* ms, int64_t* me) {
    int64_t start = from;
    while (start <= r->tlen) {
        int64_t end = 0;
        if (vayu_rx_here(r, 0, start, &end)) {
            *ms = start; *me = end;
            return 1;
        }
        start++;
    }
    return 0;
}
int64_t vayu_regex_match(VayuStr* pat, VayuStr* s) {
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t end = 0;
    if (!vayu_rx_here(&r, 0, 0, &end)) return 0;
    return end == s->len ? 1 : 0;
}
int64_t vayu_regex_search(VayuStr* pat, VayuStr* s) {
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t ms = 0, me = 0;
    if (!vayu_rx_find(&r, 0, &ms, &me)) return -1;
    return ms;
}
VayuList* vayu_regex_find_all(VayuStr* pat, VayuStr* s) {
    VayuList* out = vayu_list_new();
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t from = 0;
    while (from <= r.tlen) {
        int64_t ms = 0, me = 0;
        if (!vayu_rx_find(&r, from, &ms, &me)) break;
        vayu_list_push(out, (int64_t)vayu_mkstr(s->data + ms, me - ms));
        from = (me > ms) ? me : me + 1;
    }
    return out;
}
VayuStr* vayu_regex_replace(VayuStr* pat, VayuStr* s, VayuStr* repl) {
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    VayuList* chunks = vayu_list_new();
    int64_t pos = 0;
    int64_t from = 0;
    while (from <= r.tlen) {
        int64_t ms = 0, me = 0;
        if (!vayu_rx_find(&r, from, &ms, &me)) break;
        if (ms > pos)
            vayu_list_push(chunks, (int64_t)vayu_mkstr(s->data + pos, ms - pos));
        vayu_list_push(chunks, (int64_t)vayu_mkstr(repl->data, repl->len));
        pos = me;
        from = (me > ms) ? me : me + 1;
    }
    if (pos < s->len)
        vayu_list_push(chunks, (int64_t)vayu_mkstr(s->data + pos, s->len - pos));
    int64_t total = 0;
    for (int64_t i = 0; i < chunks->len; i++)
        total += ((VayuStr*)chunks->items[i])->len;
    VayuStr* out = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)total + 1);
    out->len = total;
    int64_t op = 0;
    for (int64_t i = 0; i < chunks->len; i++) {
        VayuStr* c = (VayuStr*)chunks->items[i];
        memcpy(out->data + op, c->data, (size_t)c->len);
        op += c->len;
    }
    out->data[total] = 0;
    return out;
}
VayuList* vayu_regex_split(VayuStr* pat, VayuStr* s) {
    VayuList* out = vayu_list_new();
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t pos = 0;
    int64_t from = 0;
    while (from <= r.tlen) {
        int64_t ms = 0, me = 0;
        if (!vayu_rx_find(&r, from, &ms, &me)) break;
        vayu_list_push(out, (int64_t)vayu_mkstr(s->data + pos, ms - pos));
        pos = me;
        from = (me > ms) ? me : me + 1;
    }
    vayu_list_push(out, (int64_t)vayu_mkstr(s->data + pos, s->len - pos));
    return out;
}

// ---- thread ----
typedef int64_t (*vayu_thread_fn_t)(int64_t);
typedef struct {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t tid;
#endif
    int64_t arg;
    int64_t result;
    vayu_thread_fn_t fn;
} VayuThread;
#ifdef _WIN32
static DWORD WINAPI vayu_thread_win_proc(LPVOID p) {
    VayuThread* t = (VayuThread*)p;
    t->result = t->fn(t->arg);
    return 0;
}
#else
static void* vayu_thread_posix_proc(void* p) {
    VayuThread* t = (VayuThread*)p;
    t->result = t->fn(t->arg);
    return NULL;
}
#endif
int64_t vayu_thread_spawn(void* fn, int64_t arg) {
    VayuThread* t = (VayuThread*)malloc(sizeof(VayuThread));
    t->fn = (vayu_thread_fn_t)fn;
    t->arg = arg;
    t->result = 0;
#ifdef _WIN32
    t->handle = CreateThread(NULL, 0, vayu_thread_win_proc, t, 0, NULL);
    if (!t->handle) { free(t); return 0; }
#else
    if (pthread_create(&t->tid, NULL, vayu_thread_posix_proc, t) != 0) {
        free(t); return 0;
    }
#endif
    return (int64_t)t;
}
int64_t vayu_thread_join(int64_t handle) {
    VayuThread* t = (VayuThread*)handle;
    if (!t) return 0;
#ifdef _WIN32
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
#else
    pthread_join(t->tid, NULL);
#endif
    int64_t r = t->result;
    free(t);
    return r;
}
int64_t vayu_thread_id(void) {
#ifdef _WIN32
    return (int64_t)GetCurrentThreadId();
#else
    return (int64_t)(uintptr_t)pthread_self();
#endif
}
typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} VayuMutex;
int64_t vayu_mutex_new(void) {
    VayuMutex* m = (VayuMutex*)malloc(sizeof(VayuMutex));
#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutex_init(&m->mtx, NULL);
#endif
    return (int64_t)m;
}
void vayu_mutex_lock(int64_t h) {
    VayuMutex* m = (VayuMutex*)h;
#ifdef _WIN32
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->mtx);
#endif
}
void vayu_mutex_unlock(int64_t h) {
    VayuMutex* m = (VayuMutex*)h;
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif
}

// ---- net ----
#ifdef _WIN32
typedef SOCKET vayu_socket_t;
#  define VAYU_INVALID_SOCKET INVALID_SOCKET
#  define VAYU_CLOSE_SOCKET   closesocket
static int vayu_net_init_done = 0;
static int vayu_net_init(void) {
    if (vayu_net_init_done) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    vayu_net_init_done = 1;
    return 0;
}
#else
typedef int vayu_socket_t;
#  define VAYU_INVALID_SOCKET (-1)
#  define VAYU_CLOSE_SOCKET   close
static int vayu_net_init(void) { return 0; }
#endif
int64_t vayu_net_listen(int64_t port) {
    if (vayu_net_init() != 0) return 0;
    vayu_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == VAYU_INVALID_SOCKET) return 0;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        VAYU_CLOSE_SOCKET(s); return 0;
    }
    if (listen(s, 16) != 0) {
        VAYU_CLOSE_SOCKET(s); return 0;
    }
    return (int64_t)s;
}
int64_t vayu_net_server_port(int64_t h) {
    vayu_socket_t s = (vayu_socket_t)h;
    struct sockaddr_in addr;
#ifdef _WIN32
    int alen = sizeof(addr);
#else
    socklen_t alen = sizeof(addr);
#endif
    if (getsockname(s, (struct sockaddr*)&addr, &alen) != 0) return 0;
    return (int64_t)ntohs(addr.sin_port);
}
int64_t vayu_net_accept(int64_t srv) {
    vayu_socket_t s = (vayu_socket_t)srv;
    vayu_socket_t c = accept(s, NULL, NULL);
    if (c == VAYU_INVALID_SOCKET) return 0;
    return (int64_t)c;
}
int64_t vayu_net_connect(VayuStr* host, int64_t port) {
    if (vayu_net_init() != 0) return 0;
    vayu_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == VAYU_INVALID_SOCKET) return 0;
    char hostbuf[256];
    int64_t n = host->len < 255 ? host->len : 255;
    memcpy(hostbuf, host->data, (size_t)n);
    hostbuf[n] = 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, hostbuf, &addr.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostbuf, NULL, &hints, &res) != 0 || !res) {
            VAYU_CLOSE_SOCKET(s); return 0;
        }
        addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        VAYU_CLOSE_SOCKET(s); return 0;
    }
    return (int64_t)s;
}
int64_t vayu_net_send(int64_t h, VayuStr* s) {
    vayu_socket_t sock = (vayu_socket_t)h;
    int64_t sent = 0;
    while (sent < s->len) {
        int n = send(sock, s->data + sent, (int)(s->len - sent), 0);
        if (n <= 0) return sent;
        sent += n;
    }
    return sent;
}
VayuStr* vayu_net_recv(int64_t h, int64_t maxlen) {
    vayu_socket_t sock = (vayu_socket_t)h;
    if (maxlen <= 0) maxlen = 4096;
    if (maxlen > 65536) maxlen = 65536;
    char* buf = (char*)malloc((size_t)maxlen);
    int n = recv(sock, buf, (int)maxlen, 0);
    if (n <= 0) { free(buf); return vayu_mkstr("", 0); }
    VayuStr* r = vayu_mkstr(buf, n);
    free(buf);
    return r;
}
int64_t vayu_net_send_line(int64_t h, VayuStr* s) {
    int64_t r = vayu_net_send(h, s);
    vayu_socket_t sock = (vayu_socket_t)h;
    send(sock, "\n", 1, 0);
    return r;
}
VayuStr* vayu_net_recv_line(int64_t h) {
    vayu_socket_t sock = (vayu_socket_t)h;
    size_t cap = 128, len = 0;
    char* buf = (char*)malloc(cap);
    char c;
    while (1) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = c;
    }
    VayuStr* r = vayu_mkstr(buf, (int64_t)len);
    free(buf);
    return r;
}
void vayu_net_close(int64_t h) {
    if (!h) return;
    vayu_socket_t s = (vayu_socket_t)h;
    VAYU_CLOSE_SOCKET(s);
}

// ---- crypto ----
static VayuStr* vayu_hex(const uint8_t* b, int64_t n) {
    static const char hx[] = "0123456789abcdef";
    char* buf = (char*)malloc((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        buf[i*2]   = hx[(b[i] >> 4) & 0xf];
        buf[i*2+1] = hx[b[i] & 0xf];
    }
    VayuStr* r = vayu_mkstr(buf, n * 2);
    free(buf);
    return r;
}
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} VayuSHA256Ctx;
static const uint32_t vayu_sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define VAYU_ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void vayu_sha256_transform(VayuSHA256Ctx* ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i, j;
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | (uint32_t)data[j+3];
    for (; i < 64; ++i) {
        uint32_t s0 = VAYU_ROTR32(m[i-15], 7) ^ VAYU_ROTR32(m[i-15], 18) ^ (m[i-15] >> 3);
        uint32_t s1 = VAYU_ROTR32(m[i-2], 17) ^ VAYU_ROTR32(m[i-2], 19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        uint32_t S1 = VAYU_ROTR32(e, 6) ^ VAYU_ROTR32(e, 11) ^ VAYU_ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + vayu_sha256_k[i] + m[i];
        uint32_t S0 = VAYU_ROTR32(a, 2) ^ VAYU_ROTR32(a, 13) ^ VAYU_ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}
static void vayu_sha256_init(VayuSHA256Ctx* ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}
static void vayu_sha256_update(VayuSHA256Ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            vayu_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}
static void vayu_sha256_final(VayuSHA256Ctx* ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0;
        vayu_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    vayu_sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4]  = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8]  = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}
typedef struct {
    uint32_t state[4];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} VayuMD5Ctx;
static const uint32_t vayu_md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
    0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
    0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
    0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
    0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
    0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const uint32_t vayu_md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
#define VAYU_ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void vayu_md5_transform(VayuMD5Ctx* ctx, const uint8_t data[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t)data[i*4] | ((uint32_t)data[i*4+1] << 8) |
               ((uint32_t)data[i*4+2] << 16) | ((uint32_t)data[i*4+3] << 24);
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) { f = (b & c) | ((~b) & d); g = i; }
        else if (i < 32) { f = (d & b) | ((~d) & c); g = (5*i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) % 16; }
        else { f = c ^ (b | (~d)); g = (7*i) % 16; }
        uint32_t tmp = d;
        d = c; c = b;
        b = b + VAYU_ROTL32(a + f + vayu_md5_k[i] + m[g], vayu_md5_s[i]);
        a = tmp;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
}
static void vayu_md5_init(VayuMD5Ctx* ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}
static void vayu_md5_update(VayuMD5Ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            vayu_md5_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}
static void vayu_md5_final(VayuMD5Ctx* ctx, uint8_t hash[16]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0;
        vayu_md5_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[56] = (uint8_t)(ctx->bitlen);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[63] = (uint8_t)(ctx->bitlen >> 56);
    vayu_md5_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)((ctx->state[0] >> (i * 8)) & 0xff);
        hash[i + 4]  = (uint8_t)((ctx->state[1] >> (i * 8)) & 0xff);
        hash[i + 8]  = (uint8_t)((ctx->state[2] >> (i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (i * 8)) & 0xff);
    }
}
VayuStr* vayu_crypto_sha256(VayuStr* s) {
    VayuSHA256Ctx ctx;
    vayu_sha256_init(&ctx);
    vayu_sha256_update(&ctx, (const uint8_t*)s->data, (size_t)s->len);
    uint8_t h[32];
    vayu_sha256_final(&ctx, h);
    return vayu_hex(h, 32);
}
VayuStr* vayu_crypto_md5(VayuStr* s) {
    VayuMD5Ctx ctx;
    vayu_md5_init(&ctx);
    vayu_md5_update(&ctx, (const uint8_t*)s->data, (size_t)s->len);
    uint8_t h[16];
    vayu_md5_final(&ctx, h);
    return vayu_hex(h, 16);
}
VayuStr* vayu_crypto_hmac_sha256(VayuStr* key, VayuStr* msg) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (key->len > 64) {
        VayuSHA256Ctx kctx;
        vayu_sha256_init(&kctx);
        vayu_sha256_update(&kctx, (const uint8_t*)key->data, (size_t)key->len);
        vayu_sha256_final(&kctx, k);
    } else {
        memcpy(k, key->data, (size_t)key->len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    VayuSHA256Ctx c;
    uint8_t inner[32], out[32];
    vayu_sha256_init(&c);
    vayu_sha256_update(&c, ipad, 64);
    vayu_sha256_update(&c, (const uint8_t*)msg->data, (size_t)msg->len);
    vayu_sha256_final(&c, inner);
    vayu_sha256_init(&c);
    vayu_sha256_update(&c, opad, 64);
    vayu_sha256_update(&c, inner, 32);
    vayu_sha256_final(&c, out);
    return vayu_hex(out, 32);
}
static const char vayu_b64e[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
VayuStr* vayu_crypto_base64_encode(VayuStr* s) {
    int64_t n = s->len;
    int64_t outLen = ((n + 2) / 3) * 4;
    char* out = (char*)malloc((size_t)outLen + 1);
    int64_t i = 0, o = 0;
    while (i + 2 < n) {
        uint32_t v = ((uint8_t)s->data[i] << 16) |
                     ((uint8_t)s->data[i+1] << 8) |
                     ((uint8_t)s->data[i+2]);
        out[o++] = vayu_b64e[(v >> 18) & 0x3F];
        out[o++] = vayu_b64e[(v >> 12) & 0x3F];
        out[o++] = vayu_b64e[(v >> 6) & 0x3F];
        out[o++] = vayu_b64e[v & 0x3F];
        i += 3;
    }
    if (i < n) {
        uint32_t v = ((uint8_t)s->data[i]) << 16;
        if (i + 1 < n) v |= ((uint8_t)s->data[i+1]) << 8;
        out[o++] = vayu_b64e[(v >> 18) & 0x3F];
        out[o++] = vayu_b64e[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? vayu_b64e[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    VayuStr* r = vayu_mkstr(out, o);
    free(out);
    return r;
}
static int vayu_b64_dec_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
VayuStr* vayu_crypto_base64_decode(VayuStr* s) {
    int64_t n = s->len;
    char* buf = (char*)malloc((size_t)n + 1);
    size_t len = 0;
    for (int64_t i = 0; i < n; ++i) {
        char c = s->data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        buf[len++] = c;
    }
    buf[len] = 0;
    size_t outCap = (len / 4) * 3 + 3;
    char* out = (char*)malloc(outCap);
    size_t oi = 0;
    size_t i = 0;
    while (i + 3 < len) {
        int v0 = vayu_b64_dec_char(buf[i]);
        int v1 = vayu_b64_dec_char(buf[i+1]);
        int v2 = (buf[i+2] == '=') ? -2 : vayu_b64_dec_char(buf[i+2]);
        int v3 = (buf[i+3] == '=') ? -2 : vayu_b64_dec_char(buf[i+3]);
        if (v0 < 0 || v1 < 0) break;
        uint32_t v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
        if (v2 >= 0) v |= ((uint32_t)v2 << 6);
        if (v3 >= 0) v |= (uint32_t)v3;
        out[oi++] = (char)((v >> 16) & 0xff);
        if (v2 >= 0) out[oi++] = (char)((v >> 8) & 0xff);
        if (v3 >= 0) out[oi++] = (char)(v & 0xff);
        i += 4;
    }
    VayuStr* r = vayu_mkstr(out, oi);
    free(out); free(buf);
    return r;
}
VayuStr* vayu_crypto_random_bytes(int64_t n) {
    if (n <= 0) return vayu_mkstr("", 0);
    char* buf = (char*)malloc((size_t)n);
#ifdef _WIN32
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (int64_t i = 0; i < n; ++i) buf[i] = (char)(rand() & 0xff);
    }
#else
    {
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t got = fread(buf, 1, (size_t)n, f);
            if (got < (size_t)n) {
                for (int64_t i = (int64_t)got; i < n; ++i) buf[i] = (char)(rand() & 0xff);
            }
            fclose(f);
        } else {
            for (int64_t i = 0; i < n; ++i) buf[i] = (char)(rand() & 0xff);
        }
    }
#endif
    VayuStr* r = vayu_mkstr(buf, n);
    free(buf);
    return r;
}

// ---- random ----
static int vayu_rand_seeded = 0;
void vayu_random_seed(int64_t s) { srand((unsigned)s); vayu_rand_seeded = 1; }
static void vayu_random_ensure_seed(void) {
    if (!vayu_rand_seeded) { srand((unsigned)time(NULL)); vayu_rand_seeded = 1; }
}
int64_t vayu_random_randint(int64_t lo, int64_t hi) {
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    vayu_random_ensure_seed();
    int64_t span = hi - lo + 1;
    if (span <= 0) return lo;
    return lo + (int64_t)(rand() % span);
}
int64_t vayu_random_randrange(int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    vayu_random_ensure_seed();
    return lo + (int64_t)(rand() % (hi - lo));
}
int64_t vayu_random_choice(VayuList* lst) {
    if (!lst || lst->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("random.choice: empty list"));
    }
    vayu_random_ensure_seed();
    return lst->items[rand() % lst->len];
}
void vayu_random_shuffle(VayuList* lst) {
    if (!lst || lst->len <= 1) return;
    vayu_random_ensure_seed();
    for (int64_t i = lst->len - 1; i > 0; --i) {
        int64_t j = rand() % (i + 1);
        int64_t t = lst->items[i];
        lst->items[i] = lst->items[j];
        lst->items[j] = t;
    }
}
VayuList* vayu_random_sample(VayuList* lst, int64_t k) {
    if (!lst || lst->len == 0 || k <= 0) return vayu_list_new();
    if (k > lst->len) k = lst->len;
    vayu_random_ensure_seed();
    int64_t* idx = (int64_t*)malloc(sizeof(int64_t) * (size_t)lst->len);
    for (int64_t i = 0; i < lst->len; ++i) idx[i] = i;
    for (int64_t i = lst->len - 1; i > 0; --i) {
        int64_t j = rand() % (i + 1);
        int64_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    VayuList* out = vayu_list_new();
    for (int64_t i = 0; i < k; ++i)
        vayu_list_push(out, lst->items[idx[i]]);
    free(idx);
    return out;
}

// ---- os ----
VayuStr* vayu_os_getenv(VayuStr* name) {
    char namebuf[256];
    int64_t n = name->len < 255 ? name->len : 255;
    memcpy(namebuf, name->data, (size_t)n);
    namebuf[n] = 0;
    const char* v = getenv(namebuf);
    if (!v) return vayu_mkstr("", 0);
    return vayu_mkstr_c(v);
}
void vayu_os_setenv(VayuStr* name, VayuStr* val) {
    char namebuf[256], valbuf[4096];
    int64_t n = name->len < 255 ? name->len : 255;
    int64_t m = val->len < 4095 ? val->len : 4095;
    memcpy(namebuf, name->data, (size_t)n); namebuf[n] = 0;
    memcpy(valbuf, val->data, (size_t)m);  valbuf[m] = 0;
#ifdef _WIN32
    _putenv_s(namebuf, valbuf);
#else
    setenv(namebuf, valbuf, 1);
#endif
}
VayuStr* vayu_os_platform(void) {
#ifdef _WIN32
    return vayu_mkstr_c("windows");
#elif defined(__APPLE__)
    return vayu_mkstr_c("macos");
#elif defined(__linux__)
    return vayu_mkstr_c("linux");
#else
    return vayu_mkstr_c("unknown");
#endif
}
VayuStr* vayu_os_hostname(void) {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) return vayu_mkstr("", 0);
    buf[sizeof(buf) - 1] = 0;
    return vayu_mkstr_c(buf);
}
VayuStr* vayu_os_cwd(void) {
    char buf[4096];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#endif
    return vayu_mkstr_c(buf);
}
void vayu_os_chdir(VayuStr* path) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;
#ifdef _WIN32
    _chdir(buf);
#else
    chdir(buf);
#endif
}
void vayu_os_exit(int64_t code) { exit((int)code); }

// ---- Phase 11.1k4: native generators ----

typedef struct VayuGen {
#ifdef _WIN32
    HANDLE                worker;
    CRITICAL_SECTION      mtx;
    CONDITION_VARIABLE    cv;
#else
    pthread_t             worker;
    pthread_mutex_t       mtx;
    pthread_cond_t        cv;
#endif
    int                   state;   /* 0=fresh, 1=running, 2=suspended, 3=done */
    int                   resume;
    int                   cancel;
    int64_t               yielded;
    int                   has_error;
    VayuExc*              error;
    int64_t             (*fn)(int64_t);
    int64_t               arg;
} VayuGen;

static VAYU_THREAD_LOCAL VayuGen* tls_current_gen = NULL;

#ifdef _WIN32
#  define VG_LOCK(g)      EnterCriticalSection(&(g)->mtx)
#  define VG_UNLOCK(g)    LeaveCriticalSection(&(g)->mtx)
#  define VG_WAIT(g)      SleepConditionVariableCS(&(g)->cv, &(g)->mtx, INFINITE)
#  define VG_SIGNAL(g)    WakeAllConditionVariable(&(g)->cv)
#else
#  define VG_LOCK(g)      pthread_mutex_lock(&(g)->mtx)
#  define VG_UNLOCK(g)    pthread_mutex_unlock(&(g)->mtx)
#  define VG_WAIT(g)      pthread_cond_wait(&(g)->cv, &(g)->mtx)
#  define VG_SIGNAL(g)    pthread_cond_broadcast(&(g)->cv)
#endif

static void vayu_gen_worker_body(VayuGen* g) {
    tls_current_gen = g;

    VG_LOCK(g);
    while (!g->resume && !g->cancel) VG_WAIT(g);
    if (g->cancel) {
        g->state = 3;
        VG_SIGNAL(g);
        VG_UNLOCK(g);
        return;
    }
    g->resume = 0;
    g->state = 1;
    VG_UNLOCK(g);

    int fid = vayu_try_push();
    if (setjmp(g_jmpBufs[fid]) == 0) {
        g->fn(g->arg);
    } else {
        g->has_error = 1;
        g->error = g_excValue;
        g_excValue = NULL;
        vayu_try_pop();
    }

    VG_LOCK(g);
    g->state = 3;
    VG_SIGNAL(g);
    VG_UNLOCK(g);
}

#ifdef _WIN32
static DWORD WINAPI vayu_gen_worker_win(LPVOID p) {
    vayu_gen_worker_body((VayuGen*)p);
    return 0;
}
#else
static void* vayu_gen_worker_posix(void* p) {
    vayu_gen_worker_body((VayuGen*)p);
    return NULL;
}
#endif

VayuGen* vayu_gen_new(int64_t (*fn)(int64_t), int64_t arg) {
    VayuGen* g = (VayuGen*)malloc(sizeof(VayuGen));
#ifdef _WIN32
    InitializeCriticalSection(&g->mtx);
    InitializeConditionVariable(&g->cv);
#else
    pthread_mutex_init(&g->mtx, NULL);
    pthread_cond_init(&g->cv, NULL);
#endif
    g->state = 0;
    g->resume = 0;
    g->cancel = 0;
    g->yielded = 0;
    g->has_error = 0;
    g->error = NULL;
    g->fn = fn;
    g->arg = arg;
#ifdef _WIN32
    g->worker = CreateThread(NULL, 0, vayu_gen_worker_win, g, 0, NULL);
#else
    pthread_create(&g->worker, NULL, vayu_gen_worker_posix, g);
#endif
    return g;
}

int64_t vayu_gen_done(VayuGen* g) {
    if (!g) return 1;
    VG_LOCK(g);
    int64_t r = (g->state == 3) ? 1 : 0;
    VG_UNLOCK(g);
    return r;
}

int64_t vayu_gen_try_next(VayuGen* g, int64_t* out) {
    VG_LOCK(g);
    if (g->state == 3) {
        if (g->has_error) {
            VayuExc* err = g->error;
            g->has_error = 0;
            g->error = NULL;
            VG_UNLOCK(g);
            vayu_raise(err);
        }
        VG_UNLOCK(g);
        return 0;
    }
    if (g->state == 1) {
        VG_UNLOCK(g);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator already running"));
    }
    g->resume = 1;
    g->state = 1;
    VG_SIGNAL(g);
    while (g->state == 1) VG_WAIT(g);
    if (g->state == 3) {
        if (g->has_error) {
            VayuExc* err = g->error;
            g->has_error = 0;
            g->error = NULL;
            VG_UNLOCK(g);
            vayu_raise(err);
        }
        VG_UNLOCK(g);
        return 0;
    }
    *out = g->yielded;
    VG_UNLOCK(g);
    return 1;
}

int64_t vayu_gen_next(VayuGen* g) {
    VG_LOCK(g);
    if (g->state == 3) {
        int has_err = g->has_error;
        VayuExc* err = g->error;
        g->has_error = 0;
        g->error = NULL;
        VG_UNLOCK(g);
        if (has_err && err) vayu_raise(err);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator exhausted"));
    }
    if (g->state == 1) {
        VG_UNLOCK(g);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator already running"));
    }
    g->resume = 1;
    g->state = 1;
    VG_SIGNAL(g);
    while (g->state == 1) VG_WAIT(g);
    if (g->state == 3) {
        int has_err = g->has_error;
        VayuExc* err = g->error;
        g->has_error = 0;
        g->error = NULL;
        VG_UNLOCK(g);
        if (has_err && err) vayu_raise(err);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator exhausted"));
    }
    int64_t v = g->yielded;
    VG_UNLOCK(g);
    return v;
}

void vayu_gen_yield(int64_t value) {
    VayuGen* g = tls_current_gen;
    if (!g) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("'yield' outside generator"));
    }
    VG_LOCK(g);
    g->yielded = value;
    g->state = 2;
    VG_SIGNAL(g);
    while (!g->resume && !g->cancel) VG_WAIT(g);
    if (g->cancel) {
        VG_UNLOCK(g);
#ifdef _WIN32
        ExitThread(0);
#else
        pthread_exit(NULL);
#endif
        return;
    }
    g->resume = 0;
    g->state = 1;
    VG_UNLOCK(g);
}

extern void vayu_main(void);

int main(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    vayu_main();
    return 0;
}
)C";

    bool NativeCompiler::writeRuntimeC(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out << kRuntimeC;
        return true;
    }

    namespace {
        void tryRemove(const std::string& p) { std::remove(p.c_str()); }
        void dumpFileHead(const std::string& path, int maxLines) {
            std::ifstream in(path);
            if (!in) return;
            std::string line; int n = 0;
            std::fprintf(stderr, "--- first %d lines of %s ---\n", maxLines, path.c_str());
            while (std::getline(in, line) && n < maxLines)
                std::fprintf(stderr, "%3d | %s\n", ++n, line.c_str());
        }
    }

    std::string NativeCompiler::buildQBE(const Block& program,
        const std::string& sourceDir) {
        QbeEmitter e;
        return e.emit(program, sourceDir);
    }

    void NativeCompiler::dumpIR(const Block& program, const std::string& sourceDir) {
        lastError_.clear();
        if (backend_ == NativeBackend::Vcb) {
            dumpIRVcb(program, sourceDir);
            return;
        }
        try { std::printf("%s\n", buildQBE(program, sourceDir).c_str()); }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "native: %s\n", e.what());
        }
    }

    int NativeCompiler::compileAndRun(const Block& program,
        const std::string& sourceDir) {
        lastError_.clear();

        if (backend_ == NativeBackend::Vcb)
            return compileAndRunVcb(program, sourceDir);

        std::string il;
        try { il = buildQBE(program, sourceDir); }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "native: %s\n", e.what());
            return 1;
        }

        /* Build into a dedicated subdirectory.  Windows occasionally holds
           a file lock for a few ms after the child .exe exits, which makes
           std::remove fail; keeping everything under _vayu_tmp/ means a
           failed delete leaves the repo root clean anyway.  clean.ps1
           wipes the whole directory on every run.

           Using std::filesystem rather than system(): the shell on this
           machine has w64devkit's sh.exe on PATH, so `if not exist` got
           parsed as POSIX and failed.  create_directories is shell-free. */
        {
            std::error_code ec;
            std::filesystem::create_directories("_vayu_tmp", ec);
        }
        /* Windows: cmd.exe treats `/` in an unquoted path as a switch
           separator, so `_vayu_tmp/foo.exe` fails with "not recognized".
           Use backslashes on Windows, forward slashes elsewhere. */
#ifdef _WIN32
        std::string base = "_vayu_tmp\\_vayu_" + std::to_string(VAYU_GETPID());
#else
        std::string base = "_vayu_tmp/_vayu_" + std::to_string(VAYU_GETPID());
#endif
        std::string ssaPath = base + ".ssa";
        std::string asmPath = base + ".s";
        std::string objPath = base + ".o";
        std::string rtPath = base + "_rt.c";
        std::string exePath = outputExe_.empty() ? (base + ".exe") : outputExe_;
        const bool  compileOnly = !outputExe_.empty();

        {
            std::ofstream out(ssaPath, std::ios::binary);
            if (!out) { lastError_ = "cannot write " + ssaPath; return 1; }
            out << il;
        }

        if (!writeRuntimeC(rtPath)) {
            lastError_ = "cannot write " + rtPath;
            tryRemove(ssaPath); return 1;
        }

        {
            std::string cmd = qbePath_ + " -t " + qbeTarget_ +
                " -o \"" + asmPath + "\" \"" + ssaPath + "\"";
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                lastError_ = "qbe failed. IL at " + ssaPath;
                std::fprintf(stderr, "native: %s\n", lastError_.c_str());
                tryRemove(rtPath); return 1;
            }
        }

        {
            std::ifstream in(asmPath, std::ios::binary);
            if (!in) { lastError_ = "cannot reopen .s"; return 1; }
            std::string filtered; std::string line;
            while (std::getline(in, line)) {
                if (line.find(".note.GNU-stack") != std::string::npos) continue;
                filtered += line; filtered += '\n';
            }
            in.close();
            std::ofstream out(asmPath, std::ios::binary);
            out << ".att_syntax prefix\n" << filtered;
        }

        {
            std::string cmd = ccPath_ + " -c -O" + std::to_string(optLevel_) +
                " \"" + asmPath + "\" -o \"" + objPath + "\"";
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                lastError_ = "assembler failed. .s at " + asmPath;
                std::fprintf(stderr, "native: %s\n", lastError_.c_str());
                dumpFileHead(asmPath, 60);
                tryRemove(ssaPath); tryRemove(rtPath); return 1;
            }
        }

        {
            std::string linkLibs;
#ifdef _WIN32
            linkLibs = " -lws2_32 -lbcrypt -luser32 -lgdi32 -lcomctl32 -lgdiplus -lole32 -luuid -lwindowscodecs -ld3d11 -ld3dcompiler";
#endif
            // Phase 15.1: extra link libraries for extern "C" functions.
            // Space-separated list; usually `-lfoo -lbar` or `.lib` paths.
            if (const char* extra = std::getenv("VAYU_FFI_LIBS")) {
                linkLibs += " ";
                linkLibs += extra;
            }
            std::string cmd = ccPath_ + " -O" + std::to_string(optLevel_) +
                " \"" + objPath + "\" \"" + rtPath +
                "\" -o \"" + exePath + "\"" + linkLibs;
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                lastError_ = "linker failed";
                std::fprintf(stderr, "native: %s\n", lastError_.c_str());
                tryRemove(ssaPath); tryRemove(asmPath);
                tryRemove(objPath); tryRemove(rtPath);
                return 1;
            }
        }

        tryRemove(ssaPath);
        tryRemove(asmPath);
        tryRemove(objPath);
        tryRemove(rtPath);

        if (compileOnly) return 0;

        int runRc = std::system(("\"" + exePath + "\"").c_str());

        bool keep = (std::getenv("VAYU_KEEP_TEMP") != nullptr);
        if (!keep) tryRemove(exePath);

        if (runRc != 0) {
            lastError_ = "program exited " + std::to_string(runRc);
            return 1;
        }
        return 0;
    }

    void NativeCompiler::dumpIRVcb(const Block& program,
        const std::string& sourceDir) {
        try {
            VcbLower lower;
            std::string ir = lower.lower(program, sourceDir);
            std::fwrite(ir.data(), 1, ir.size(), stdout);
        }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "vcb-lower: %s\n", e.what());
        }
    }

    std::string NativeCompiler::resolveVcbPath() const {
        auto exists = [](const std::string& p) -> bool {
            std::ifstream f(p, std::ios::binary);
            return (bool)f;
            };

#ifdef _WIN32
        const char* exeName = "vcb.exe";
        const char* subDir = "tools\\";
        const char* upUnit = "..\\";
#else
        const char* exeName = "vcb";
        const char* subDir = "tools/";
        const char* upUnit = "../";
#endif

        // Candidate 0: whatever the constructor stored, if it is
        // non-empty and not a bare filename.  cmd.exe resolves a
        // relative path against the cwd; the walk below handles the
        // case where the cwd is not the project root.
        if (!vcbPath_.empty()) {
            bool hasSeparator =
                vcbPath_.find('\\') != std::string::npos ||
                vcbPath_.find('/') != std::string::npos;
            if (hasSeparator && exists(vcbPath_)) return vcbPath_;
        }

        // Candidate 1: tools/ relative to cwd.
        {
            std::string cur = std::string(subDir) + exeName;
            if (exists(cur)) return cur;
        }

        // Candidates 2..7: walk up to six levels.
        for (int up = 1; up <= 6; ++up) {
            std::string prefix;
            for (int k = 0; k < up; ++k) prefix += upUnit;
            std::string cur = prefix + subDir + exeName;
            if (exists(cur)) return cur;
        }

        return {};
    }

    int NativeCompiler::compileAndRunVcb(const Block& program,
        const std::string& sourceDir) {
        std::string ir;
        try {
            VcbLower lower;
            ir = lower.lower(program, sourceDir);
        }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "vcb-lower: %s\n", e.what());
            return 1;
        }

        std::error_code ec;
        std::filesystem::create_directories("_vayu_tmp", ec);
#ifdef _WIN32
        std::string base = "_vayu_tmp\\_vayu_" + std::to_string(VAYU_GETPID());
#else
        std::string base = "_vayu_tmp/_vayu_" + std::to_string(VAYU_GETPID());
#endif
        std::string irPath = base + ".vcbir";
        std::string exePath = outputExe_.empty() ? (base + ".exe") : outputExe_;
        const bool  compileOnly = !outputExe_.empty();

        {
            std::ofstream out(irPath, std::ios::binary);
            if (!out) { lastError_ = "cannot write " + irPath; return 1; }
            out << ir;
        }

        // Print what was actually configured so the next failure of
        // this kind takes one glance to diagnose instead of three
        // round trips.
        if (const char* env = std::getenv("VAYU_VCB"))
            std::fprintf(stderr, "native: VAYU_VCB=%s\n", env);
        std::fprintf(stderr, "native: vcbPath_=%s\n", vcbPath_.c_str());

        std::string vcbExe = resolveVcbPath();
        if (vcbExe.empty()) {
            lastError_ = "cannot find vcb.exe.  Place it at "
                "tools\\vcb.exe relative to the project root, "
                "or set VAYU_VCB to a path containing '\\' or '/'.";
            std::fprintf(stderr, "native: %s\n", lastError_.c_str());
            return 1;
        }
        std::fprintf(stderr, "native: resolved vcb=%s\n", vcbExe.c_str());

        // Build the subprocess command.
        //
        // cmd.exe's /c parser has a documented quirk: if the command
        // line begins with a quote AND contains more than two quote
        // characters, cmd strips the *first* and *last* quote on the
        // whole line.  With a quoted exe path at the front and a
        // quoted output path at the end, that eats the closing quote
        // on the output and leaves a stray " on the executable name,
        // producing ERROR_PATH_NOT_FOUND ("The system cannot find the
        // path specified.").  Since vcbExe is a project-relative path
        // we control, it never contains spaces, so we leave it
        // unquoted.  If a caller sets VAYU_VCB to a path with a space,
        // prefix with `call ` so cmd.exe never sees a leading quote.
        std::string exeCmd;
        if (vcbExe.find(' ') == std::string::npos) {
            exeCmd = vcbExe;
        }
        else {
            exeCmd = "call \"" + vcbExe + "\"";
        }
        std::string cmd = exeCmd + " build \"" + irPath +
            "\" -o \"" + exePath + "\"";
        std::fprintf(stderr, "native: running %s\n", cmd.c_str());
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            lastError_ = "vcb failed (exit " + std::to_string(rc) +
                "). IR at " + irPath;
            std::fprintf(stderr, "native: %s\n", lastError_.c_str());
            return 1;
        }

        tryRemove(irPath);

        if (compileOnly) return 0;

        int runRc = std::system(("\"" + exePath + "\"").c_str());
        bool keep = (std::getenv("VAYU_KEEP_TEMP") != nullptr);
        if (!keep) tryRemove(exePath);
        if (runRc != 0) {
            lastError_ = "program exited " + std::to_string(runRc);
            return 1;
        }
        return 0;
    }

} // namespace vayu