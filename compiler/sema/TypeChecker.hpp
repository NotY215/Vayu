#pragma once
#include "types/Type.hpp"
#include "ast/Ast.hpp"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vayu {

    class TypeError : public std::runtime_error {
    public:
        SourceLocation loc;
        TypeError(std::string msg, SourceLocation l)
            : std::runtime_error(std::move(msg)), loc(l) {}
    };

    class TypeChecker {
    public:
        void check(const Block& program);

    private:
        struct Scope { std::unordered_map<std::string, TypePtr> vars; };

        std::vector<Scope>                       scopes_;
        std::unordered_map<std::string, TypePtr> functions_;
        std::unordered_map<std::string, TypePtr> structs_;
        std::unordered_map<std::string, TypePtr> builtins_;

        std::unordered_set<std::string>          consts_;

        std::unordered_map<std::string,
            std::unordered_map<std::string, long long>> enums_;

        std::unordered_map<std::string,
            std::unordered_map<std::string, TypePtr>> statics_;

        // Phase 11.2 — stack of type-parameter scopes.  Each scope maps a
        // user-visible name (`T`) to its TypeParam node.  Pushed on entering
        // a generic signature or class body.
        std::vector<std::unordered_map<std::string, TypePtr>> typeParamScopes_;
        int nextTypeParamId_ = 0;

        TypePtr currentReturnType_;
        TypePtr currentClass_;
        int     loopDepth_ = 0;

        void    collectSignatures(const Block& program);
        void    collectDefs(const Block& block, bool isTopLevel);

        void    checkStmt(const Stmt* s);
        void    checkBlock(const Block& b);
        TypePtr checkExpr(const Expr* e);

        void    pushScope();
        void    popScope();
        void    defineVar(const std::string& name, TypePtr t);

        TypePtr lookupUserVar(const std::string& name);
        TypePtr lookupVar(const std::string& name);

        TypePtr resolveTypeExpr(const Expr* e);

        TypePtr checkStructConstruction(const StructStmt* decl, const CallExpr* call,
            const std::string& name);
        void    checkMethodBody(const DefStmt* m, TypePtr cls);

        TypePtr lookupCollectionMethod(const TypePtr& target, const std::string& name,
            SourceLocation loc);
        TypePtr commonElementType(const TypePtr& a, const TypePtr& b, SourceLocation loc);

        bool    unify(const TypePtr& pattern, const TypePtr& actual,
            std::unordered_map<std::string, TypePtr>& subst);
        TypePtr substitute(const TypePtr& t,
            const std::unordered_map<std::string, TypePtr>& subst);

        void checkVisibility(const std::string& declClass,
            Visibility vis,
            const std::string& member,
            SourceLocation loc);
        int visCode(Visibility v) {
            switch (v) {
            case Visibility::Public:    return 0;
            case Visibility::Protected: return 1;
            case Visibility::Private:   return 2;
            }
            return 0;
        }
        bool isSubclassOf(TypePtr sub, TypePtr base) const;

        [[noreturn]] void error(SourceLocation loc, const std::string& msg);
        void installBuiltins();
        void installBuiltinExceptions();
    };

} // namespace vayu