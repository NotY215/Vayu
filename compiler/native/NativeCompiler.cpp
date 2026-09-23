#include "NativeCompiler.hpp"
#include "parser/Parser.hpp"
#include "lexer/Lexer.hpp"
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
        if (const char* p = std::getenv("VAYU_CC_OPT")) {
            int n = std::atoi(p);
            if (n >= 0 && n <= 3) optLevel_ = n;
        }
        optLevel_ = 2;   // -O2: fastest runtime.  Compile time is ~1.4x -O1.
    }

    namespace {

        enum class VType {
            Int, Bool, Str, List, Map, Obj, Exc, Void, Unknown,
            Tuple, Set, Ptr
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
                    for (auto& s : kv.second.stmts) {
                        if (s->kind != StmtKind::Def) continue;
                        emitFunction(static_cast<const DefStmt*>(s.get()), nullptr, prefix);
                        raw("");
                    }
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
                            n->moduleName != "gui" && !modules_.count(n->moduleName))
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

                currentModulePrefix_ = prefix;
                for (auto& s : modBlock.stmts) {
                    if (s->kind == StmtKind::Def) continue;
                    if (s->kind == StmtKind::Struct || s->kind == StmtKind::Class) continue;
                    if (s->kind == StmtKind::Enum) continue;
                    if (s->kind == StmtKind::Import || s->kind == StmtKind::FromImport) continue;
                    emitStmt(s.get());
                }
                currentModulePrefix_.clear();
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
                case VType::Int:  return 0;
                case VType::Bool: return 1;
                case VType::Str:  return 2;
                default:          return 0;
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
                    else if (s == "float") v.type = VType::Int;
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
                case ExprKind::FloatLit: r.type = VType::Int;  break;
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
                        Val a = emitExpr(n->lhs.get());
                        Val b = emitExpr(n->rhs.get());
                        std::string ab = newTemp(); line(ab + " =w cnel " + a.ssa + ", 0");
                        std::string bb = newTemp(); line(bb + " =w cnel " + b.ssa + ", 0");
                        std::string w = newTemp();
                        line(w + " =w " + (n->op == BinOp::And ? "and" : "or") +
                            " " + ab + ", " + bb);
                        std::string ext = newTemp();
                        line(ext + " =l extsw " + w);
                        r.ssa = ext; r.type = VType::Bool; return r;
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
                case ExprKind::FloatLit:
                    throw std::runtime_error("native: floats not yet supported");
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
                    if (s == "float") return VType::Int;
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
                if (m == "listbox_set_index") { Val e = a0(); Val i = a1(); line("call $vayu_gui_listbox_set_index(l " + e.ssa + ", l " + i.ssa + ")"); r.ssa = "0"; r.type = VType::Void; return r; }

                throw std::runtime_error("native: gui has no method '" + m + "'");
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
                    return v;
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
                std::string fnName = name;
                if (fi != fromImports_.end())
                    fnName = mangle(fi->second) + "_" + name;

                auto fit = topFnDecls_.find(name);

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
                std::string t = newTemp();
                line(t + " =l call $vayu_fn_" + mangle(fnName) + "(" + argsStr + ")");
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
                        std::string w2 = emitCond(b->rhs.get());
                        std::string w = newTemp();
                        line(w + " =w and " + w1 + ", " + w2);
                        return w;
                    }
                    case BinOp::Or: {
                        std::string w1 = emitCond(b->lhs.get());
                        std::string w2 = emitCond(b->rhs.get());
                        std::string w = newTemp();
                        line(w + " =w or " + w1 + ", " + w2);
                        return w;
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
        try { std::printf("%s\n", buildQBE(program, sourceDir).c_str()); }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "native: %s\n", e.what());
        }
    }

    int NativeCompiler::compileAndRun(const Block& program,
        const std::string& sourceDir) {
        lastError_.clear();

        std::string il;
        try { il = buildQBE(program, sourceDir); }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "native: %s\n", e.what());
            return 1;
        }

        std::string base = "_vayu_" + std::to_string(VAYU_GETPID());
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
            linkLibs = " -lws2_32 -lbcrypt -luser32 -lgdi32 -lcomctl32";
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

} // namespace vayu