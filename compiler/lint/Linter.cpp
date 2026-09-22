// compiler/lint/Linter.cpp
#include "Linter.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "ast/Ast.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vayu::lint {

    namespace {

        struct Scope {
            std::unordered_map<std::string, SourceLocation> defs;
            std::unordered_set<std::string> used;
        };

        struct Ctx {
            std::vector<Scope>              scopes;
            std::vector<Diagnostic>* diags = nullptr;
            Options                         opts;
            std::unordered_set<std::string> builtins;

            void push() { scopes.emplace_back(); }
            void pop() { scopes.pop_back(); }

            void define(const std::string& name, SourceLocation loc) {
                if (scopes.empty()) push();
                if (scopes.back().defs.count(name)) return;   // keep first
                scopes.back().defs[name] = loc;
            }

            void use(const std::string& name) {
                // mark on all enclosing scopes so nested reads count
                for (auto& s : scopes) s.used.insert(name);
            }
        };

        void walkExpr(const Expr* e, Ctx& c);
        void walkStmt(const Stmt* s, Ctx& c);
        void walkBlock(const Block& b, Ctx& c);

        void walkExpr(const Expr* e, Ctx& c) {
            if (!e) return;
            switch (e->kind) {
            case ExprKind::NameRef: {
                auto* n = static_cast<const NameRefExpr*>(e);
                c.use(n->name);
                break;
            }
            case ExprKind::Unary:
                walkExpr(static_cast<const UnaryExpr*>(e)->operand.get(), c);
                break;
            case ExprKind::Binary: {
                auto* n = static_cast<const BinaryExpr*>(e);
                walkExpr(n->lhs.get(), c);
                walkExpr(n->rhs.get(), c);
                break;
            }
            case ExprKind::Grouping:
                walkExpr(static_cast<const GroupingExpr*>(e)->inner.get(), c);
                break;
            case ExprKind::Call: {
                auto* n = static_cast<const CallExpr*>(e);
                walkExpr(n->callee.get(), c);
                for (auto& a : n->args) walkExpr(a.value.get(), c);
                break;
            }
            case ExprKind::Attr:
                walkExpr(static_cast<const AttrExpr*>(e)->target.get(), c);
                break;
            case ExprKind::Index: {
                auto* n = static_cast<const IndexExpr*>(e);
                walkExpr(n->target.get(), c);
                walkExpr(n->index.get(), c);
                break;
            }
            case ExprKind::ListLit:
                for (auto& el : static_cast<const ListLitExpr*>(e)->elements)
                    walkExpr(el.get(), c);
                break;
            case ExprKind::MapLit: {
                auto* n = static_cast<const MapLitExpr*>(e);
                for (auto& en : n->entries) {
                    walkExpr(en.key.get(), c);
                    walkExpr(en.value.get(), c);
                }
                break;
            }
            case ExprKind::TupleLit:
                for (auto& el : static_cast<const TupleLitExpr*>(e)->elements)
                    walkExpr(el.get(), c);
                break;
            case ExprKind::SetLit:
                for (auto& el : static_cast<const SetLitExpr*>(e)->elements)
                    walkExpr(el.get(), c);
                break;
            case ExprKind::Slice: {
                auto* n = static_cast<const SliceExpr*>(e);
                walkExpr(n->target.get(), c);
                if (n->start) walkExpr(n->start.get(), c);
                if (n->end)   walkExpr(n->end.get(), c);
                break;
            }
            case ExprKind::Lambda: {
                auto* n = static_cast<const LambdaExpr*>(e);
                c.push();
                for (auto& p : n->params) c.define(p, n->loc);
                walkExpr(n->body.get(), c);
                c.pop();
                break;
            }
            default: break;
            }
        }

        // Returns true if the block terminates unconditionally.
        bool terminates(const Block& b) {
            if (b.stmts.empty()) return false;
            switch (b.stmts.back()->kind) {
            case StmtKind::Return:
            case StmtKind::Raise:
            case StmtKind::Break:
            case StmtKind::Continue:
                return true;
            default:
                return false;
            }
        }

        void checkUnreachable(const Block& b, Ctx& c) {
            for (size_t i = 0; i < b.stmts.size(); ++i) {
                const Stmt* s = b.stmts[i].get();
                bool isTerm =
                    s->kind == StmtKind::Return ||
                    s->kind == StmtKind::Raise ||
                    s->kind == StmtKind::Break ||
                    s->kind == StmtKind::Continue;
                if (isTerm && i + 1 < b.stmts.size()) {
                    const Stmt* next = b.stmts[i + 1].get();
                    if (next->kind != StmtKind::Pass) {
                        c.diags->push_back({
                            next->loc.line, next->loc.column,
                            "unreachable code after '" +
                            std::string(s->kind == StmtKind::Return ? "return" :
                                        s->kind == StmtKind::Raise ? "raise" :
                                        s->kind == StmtKind::Break ? "break" :
                                                                      "continue") +
                            "'"
                            });
                    }
                }
            }
        }

        void walkBlock(const Block& b, Ctx& c) {
            if (c.opts.unreachableCode) checkUnreachable(b, c);
            for (auto& s : b.stmts) walkStmt(s.get(), c);
        }

        void walkStmt(const Stmt* s, Ctx& c) {
            if (!s) return;
            switch (s->kind) {
            case StmtKind::Expr:
                walkExpr(static_cast<const ExprStmt*>(s)->expr.get(), c);
                break;
            case StmtKind::Assign: {
                auto* n = static_cast<const AssignStmt*>(s);
                walkExpr(n->value.get(), c);
                if (n->target->kind == ExprKind::NameRef) {
                    c.define(static_cast<const NameRefExpr*>(
                        n->target.get())->name, n->loc);
                }
                else {
                    walkExpr(n->target.get(), c);
                }
                break;
            }
            case StmtKind::AnnotAssign: {
                auto* n = static_cast<const AnnotAssignStmt*>(s);
                if (n->value) walkExpr(n->value.get(), c);
                c.define(n->name, n->loc);
                break;
            }
            case StmtKind::Const: {
                auto* n = static_cast<const ConstStmt*>(s);
                walkExpr(n->value.get(), c);
                c.define(n->name, n->loc);
                break;
            }
            case StmtKind::If: {
                auto* n = static_cast<const IfStmt*>(s);
                walkExpr(n->cond.get(), c);
                c.push(); walkBlock(n->thenBody, c); c.pop();
                for (auto& ec : n->elifs) {
                    walkExpr(ec.cond.get(), c);
                    c.push(); walkBlock(ec.body, c); c.pop();
                }
                if (n->elseBody) {
                    c.push(); walkBlock(*n->elseBody, c); c.pop();
                }
                break;
            }
            case StmtKind::While: {
                auto* n = static_cast<const WhileStmt*>(s);
                walkExpr(n->cond.get(), c);
                c.push(); walkBlock(n->body, c); c.pop();
                break;
            }
            case StmtKind::For: {
                auto* n = static_cast<const ForStmt*>(s);
                walkExpr(n->iterable.get(), c);
                c.push();
                c.define(n->targetName, n->loc);
                walkBlock(n->body, c);
                c.pop();
                break;
            }
            case StmtKind::Def: {
                auto* n = static_cast<const DefStmt*>(s);
                c.push();
                for (auto& p : n->params) c.define(p.name, n->loc);
                walkBlock(n->body, c);
                if (c.opts.emptyBodies) {
                    bool empty = n->body.stmts.empty();
                    if (!empty && n->body.stmts.size() == 1) {
                        const Stmt* only = n->body.stmts[0].get();
                        if (only->kind == StmtKind::Pass) empty = true;
                    }
                    if (empty) {
                        c.diags->push_back({
                            n->loc.line, n->loc.column,
                            "function '" + n->name + "' has an empty body"
                            });
                    }
                }
                // Unused locals: any name defined in this scope but not used.
                if (c.opts.unusedLocals) {
                    for (auto& kv : c.scopes.back().defs) {
                        if (c.scopes.back().used.count(kv.first)) continue;
                        // Parameter detection: skip names in params list.
                        bool isParam = false;
                        for (auto& p : n->params)
                            if (p.name == kv.first) { isParam = true; break; }
                        if (isParam) continue;
                        c.diags->push_back({
                            kv.second.line, kv.second.column,
                            "unused local '" + kv.first + "'"
                            });
                    }
                }
                c.pop();
                break;
            }
            case StmtKind::Return: {
                auto* n = static_cast<const ReturnStmt*>(s);
                if (n->value) walkExpr(n->value.get(), c);
                break;
            }
            case StmtKind::Class: {
                auto* n = static_cast<const ClassStmt*>(s);
                for (auto& sf : n->staticFields)
                    if (sf.init) walkExpr(sf.init.get(), c);
                for (auto& m : n->methods) walkStmt(m.get(), c);
                break;
            }
            case StmtKind::Try: {
                auto* n = static_cast<const TryStmt*>(s);
                c.push(); walkBlock(n->tryBody, c); c.pop();
                for (auto& h : n->handlers) {
                    c.push();
                    if (!h.varName.empty()) c.define(h.varName, n->loc);
                    walkBlock(h.body, c);
                    c.pop();
                }
                if (n->finallyBody) {
                    c.push(); walkBlock(*n->finallyBody, c); c.pop();
                }
                break;
            }
            case StmtKind::Raise: {
                auto* n = static_cast<const RaiseStmt*>(s);
                if (n->exception) walkExpr(n->exception.get(), c);
                break;
            }
            case StmtKind::Yield: {
                auto* n = static_cast<const YieldStmt*>(s);
                if (n->value) walkExpr(n->value.get(), c);
                break;
            }
            case StmtKind::Block:
                walkBlock(static_cast<const BlockStmt*>(s)->body, c);
                break;
            default: break;
            }
        }

        void installBuiltins(Ctx& c) {
            static const char* names[] = {
                "print","str","bool","int","float","len","abs","type","min","max",
                "input","read_line","read_int","read_all","range","ord","chr",
                "list","map","filter","sorted","reduce","any","all","sum",
                "hash","id","callable","isinstance","issubclass","getattr","hasattr",
                "dir","repr","enumerate","zip","reversed","round","pow","divmod",
                "sign","gcd","lcm","clamp","comb","perm","isqrt","factorial",
                "tuple","set","setattr","delattr","next","malloc","free",
                "read_file","write_file","file_exists","args","run_command","exit",
                "print_raw","join","math","fs","time","json","regex","thread",
                "net","crypto","random","os","py",
                "Exception","ValueError","TypeError","RuntimeError",
                "ZeroDivisionError","IndexError","KeyError","NameError",
                "AttributeError",
                nullptr
            };
            for (int i = 0; names[i]; ++i) c.builtins.insert(names[i]);
        }

    } // namespace

    Result check(const std::string& source, const Options& opts) {
        Result r;

        vayu::Block program;
        try {
            vayu::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            vayu::Parser parser(std::move(tokens));
            program = parser.parseProgram();
        }
        catch (const vayu::ParseError& e) {
            r.ok = false;
            std::ostringstream ss;
            ss << "parse error at line " << e.loc.line << ":"
                << e.loc.column << ": " << e.what();
            r.error = ss.str();
            return r;
        }

        Ctx c;
        c.opts = opts;
        c.diags = &r.diagnostics;
        installBuiltins(c);
        c.push();  // top-level scope

        for (auto& s : program.stmts) walkStmt(s.get(), c);

        // Top-level unused locals (globals) — skip, they're usually public.

        c.pop();
        r.ok = true;
        return r;
    }

    int run(int argc, char** argv) {
        if (argc < 2) {
            std::cerr << "usage: vlint <file.vyu> [--json]\n";
            return 2;
        }
        std::string path;
        bool json = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--json") == 0) json = true;
            else if (argv[i][0] == '-') {
                std::cerr << "vlint: unknown flag " << argv[i] << "\n";
                return 2;
            }
            else {
                if (!path.empty()) {
                    std::cerr << "vlint: only one file at a time\n";
                    return 2;
                }
                path = argv[i];
            }
        }
        if (path.empty()) {
            std::cerr << "usage: vlint <file.vyu> [--json]\n";
            return 2;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "vlint: cannot read " << path << "\n";
            return 2;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();

        Options opts;
        auto r = check(src, opts);
        if (!r.ok) {
            std::cerr << path << ": " << r.error << "\n";
            return 1;
        }

        if (json) {
            std::cout << "[";
            for (size_t i = 0; i < r.diagnostics.size(); ++i) {
                if (i) std::cout << ",";
                std::cout << "{\"line\":" << r.diagnostics[i].line
                    << ",\"col\":" << r.diagnostics[i].col
                    << ",\"message\":\"";
                // crude escape
                for (char ch : r.diagnostics[i].message) {
                    if (ch == '"' || ch == '\\') std::cout << '\\';
                    std::cout << ch;
                }
                std::cout << "\"}";
            }
            std::cout << "]\n";
        }
        else {
            for (auto& d : r.diagnostics) {
                std::cout << path << ":" << d.line << ":" << d.col
                    << ": warning: " << d.message << "\n";
            }
        }

        return r.diagnostics.empty() ? 0 : 1;
    }

}