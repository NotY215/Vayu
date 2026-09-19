#pragma once
#include "lexer/Token.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vayu {

    // ===========================================================================
    // Expressions
    // ===========================================================================

    enum class ExprKind {
        IntLit, FloatLit, StringLit, CharLit, BoolLit, NoneLit,
        NameRef,
        Unary, Binary, Grouping,
        Call, Attr, Index,
        ListLit, MapLit,
        Lambda,
        GenericType,
        TupleLit,
        SetLit,
        Slice,
    };

    enum class BinOp {
        Add, Sub, Mul, Div, FloorDiv, Mod, Pow,
        Eq, NotEq, Lt, Gt, LtEq, GtEq,
        And, Or, In, Is,
        BAnd, BOr, BXor, Shl, Shr,
    };
    enum class UnOp {
        Neg, Pos, Not,
        BNot,
        // Phase 15.2b — raw pointers
        AddrOf,
        Deref,
    };

    struct Expr {
        ExprKind       kind;
        SourceLocation loc;
        Expr(ExprKind k, SourceLocation l) : kind(k), loc(l) {}
        virtual ~Expr() = default;
    };
    using ExprPtr = std::unique_ptr<Expr>;

    struct IntLitExpr : Expr {
        long long value; std::string text;
        IntLitExpr(long long v, std::string t, SourceLocation l)
            : Expr(ExprKind::IntLit, l), value(v), text(std::move(t)) {
        }
    };
    struct FloatLitExpr : Expr {
        double value; std::string text;
        FloatLitExpr(double v, std::string t, SourceLocation l)
            : Expr(ExprKind::FloatLit, l), value(v), text(std::move(t)) {
        }
    };
    struct StringLitExpr : Expr {
        std::string value;
        StringLitExpr(std::string v, SourceLocation l)
            : Expr(ExprKind::StringLit, l), value(std::move(v)) {
        }
    };
    struct CharLitExpr : Expr {
        std::string value;
        CharLitExpr(std::string v, SourceLocation l)
            : Expr(ExprKind::CharLit, l), value(std::move(v)) {
        }
    };
    struct BoolLitExpr : Expr {
        bool value;
        BoolLitExpr(bool v, SourceLocation l)
            : Expr(ExprKind::BoolLit, l), value(v) {
        }
    };
    struct NoneLitExpr : Expr { NoneLitExpr(SourceLocation l) : Expr(ExprKind::NoneLit, l) {} };
    struct NameRefExpr : Expr {
        std::string name;
        NameRefExpr(std::string n, SourceLocation l)
            : Expr(ExprKind::NameRef, l), name(std::move(n)) {
        }
    };
    struct UnaryExpr : Expr {
        UnOp op; ExprPtr operand;
        UnaryExpr(UnOp o, ExprPtr e, SourceLocation l)
            : Expr(ExprKind::Unary, l), op(o), operand(std::move(e)) {
        }
    };
    struct BinaryExpr : Expr {
        BinOp op; ExprPtr lhs, rhs;
        BinaryExpr(BinOp o, ExprPtr a, ExprPtr b, SourceLocation l)
            : Expr(ExprKind::Binary, l), op(o), lhs(std::move(a)), rhs(std::move(b)) {
        }
    };
    struct GroupingExpr : Expr {
        ExprPtr inner;
        GroupingExpr(ExprPtr e, SourceLocation l)
            : Expr(ExprKind::Grouping, l), inner(std::move(e)) {
        }
    };

    struct CallArg { std::string name; ExprPtr value; SourceLocation loc; };

    struct CallExpr : Expr {
        ExprPtr              callee;
        std::vector<CallArg> args;
        CallExpr(ExprPtr c, std::vector<CallArg> a, SourceLocation l)
            : Expr(ExprKind::Call, l), callee(std::move(c)), args(std::move(a)) {
        }
    };
    struct AttrExpr : Expr {
        ExprPtr target; std::string name;
        AttrExpr(ExprPtr t, std::string n, SourceLocation l)
            : Expr(ExprKind::Attr, l), target(std::move(t)), name(std::move(n)) {
        }
    };
    struct IndexExpr : Expr {
        ExprPtr target; ExprPtr index;
        IndexExpr(ExprPtr t, ExprPtr i, SourceLocation l)
            : Expr(ExprKind::Index, l), target(std::move(t)), index(std::move(i)) {
        }
    };

    struct ListLitExpr : Expr {
        std::vector<ExprPtr> elements;
        explicit ListLitExpr(SourceLocation l) : Expr(ExprKind::ListLit, l) {}
    };

    struct MapEntry { ExprPtr key; ExprPtr value; };
    struct MapLitExpr : Expr {
        std::vector<MapEntry> entries;
        explicit MapLitExpr(SourceLocation l) : Expr(ExprKind::MapLit, l) {}
    };

    // Phase 14.0: real tuple literal.
    struct TupleLitExpr : Expr {
        std::vector<ExprPtr> elements;
        explicit TupleLitExpr(SourceLocation l) : Expr(ExprKind::TupleLit, l) {}
    };

    // Phase 14.1: real set literal.
    struct SetLitExpr : Expr {
        std::vector<ExprPtr> elements;
        explicit SetLitExpr(SourceLocation l) : Expr(ExprKind::SetLit, l) {}
    };

    // Phase 14.4: `t[a:b]` / `t[a:]` / `t[:b]` / `t[:]`.
    // `start` and `end` may each be null.
    struct SliceExpr : Expr {
        ExprPtr target;
        ExprPtr start;
        ExprPtr end;
        SliceExpr(ExprPtr t, ExprPtr s, ExprPtr e, SourceLocation l)
            : Expr(ExprKind::Slice, l), target(std::move(t)),
            start(std::move(s)), end(std::move(e)) {
        }
    };

    struct LambdaExpr : Expr {
        std::vector<std::string> params;
        ExprPtr                  body;
        explicit LambdaExpr(SourceLocation l) : Expr(ExprKind::Lambda, l) {}
    };

    struct GenericTypeExpr : Expr {
        std::string          name;
        std::vector<ExprPtr> typeArgs;
        GenericTypeExpr(std::string n, SourceLocation l)
            : Expr(ExprKind::GenericType, l), name(std::move(n)) {
        }
    };

    // ===========================================================================
    // Statements
    // ===========================================================================

    struct Stmt;
    using StmtPtr = std::unique_ptr<Stmt>;
    struct Block { std::vector<StmtPtr> stmts; };

    enum class StmtKind {
        Expr, Assign, AnnotAssign,
        If, While, Def, Return,
        Struct, Class, For,
        Try, Raise,
        Import, FromImport,
        Pass, Break, Continue,
        Const,
        Enum,
        Yield,
        Block,
        Extern,
    };

    struct Stmt {
        StmtKind       kind;
        SourceLocation loc;
        Stmt(StmtKind k, SourceLocation l) : kind(k), loc(l) {}
        virtual ~Stmt() = default;
    };

    struct ExprStmt : Stmt {
        ExprPtr expr;
        ExprStmt(ExprPtr e, SourceLocation l)
            : Stmt(StmtKind::Expr, l), expr(std::move(e)) {
        }
    };
    struct AssignStmt : Stmt {
        ExprPtr target; ExprPtr value;
        AssignStmt(ExprPtr t, ExprPtr v, SourceLocation l)
            : Stmt(StmtKind::Assign, l), target(std::move(t)), value(std::move(v)) {
        }
    };
    struct AnnotAssignStmt : Stmt {
        std::string name; ExprPtr type; ExprPtr value;
        AnnotAssignStmt(std::string n, ExprPtr t, ExprPtr v, SourceLocation l)
            : Stmt(StmtKind::AnnotAssign, l),
            name(std::move(n)), type(std::move(t)), value(std::move(v)) {
        }
    };
    struct ElifClause { ExprPtr cond; Block body; };
    struct IfStmt : Stmt {
        ExprPtr cond; Block thenBody;
        std::vector<ElifClause> elifs; std::optional<Block> elseBody;
        IfStmt(ExprPtr c, Block t, SourceLocation l)
            : Stmt(StmtKind::If, l), cond(std::move(c)), thenBody(std::move(t)) {
        }
    };
    struct WhileStmt : Stmt {
        ExprPtr cond; Block body;
        WhileStmt(ExprPtr c, Block b, SourceLocation l)
            : Stmt(StmtKind::While, l), cond(std::move(c)), body(std::move(b)) {
        }
    };

    struct Param { std::string name; ExprPtr type; };

    struct DefStmt : Stmt {
        std::string               name;
        std::vector<std::string>  typeParams;
        std::vector<std::string>  typeParamConstraints;
        std::vector<Param>        params;
        ExprPtr                   returnType;
        Block                     body;
        Visibility                vis = Visibility::Public;
        bool                      isGenerator = false;
        DefStmt(std::string n, std::vector<Param> p, ExprPtr rt, Block b, SourceLocation l)
            : Stmt(StmtKind::Def, l),
            name(std::move(n)), params(std::move(p)),
            returnType(std::move(rt)), body(std::move(b)) {
        }
    };

    struct ReturnStmt : Stmt {
        ExprPtr value;
        ReturnStmt(ExprPtr v, SourceLocation l)
            : Stmt(StmtKind::Return, l), value(std::move(v)) {
        }
    };

    struct FieldDef {
        std::string name;
        ExprPtr type;
        SourceLocation loc;
        Visibility vis = Visibility::Public;
    };

    struct StructStmt : Stmt {
        std::string           name;
        std::vector<FieldDef> fields;
        StructStmt(std::string n, std::vector<FieldDef> f, SourceLocation l)
            : Stmt(StmtKind::Struct, l), name(std::move(n)), fields(std::move(f)) {
        }
    };

    struct StaticFieldDef {
        std::string    name;
        ExprPtr        type;
        ExprPtr        init;
        SourceLocation loc;
        Visibility     vis = Visibility::Public;
    };

    struct ClassStmt : Stmt {
        std::string                           name;
        std::vector<std::string>              typeParams;
        std::vector<std::string>              typeParamConstraints;
        std::string                           parentName;
        std::vector<FieldDef>                 fields;
        std::vector<StaticFieldDef>           staticFields;
        std::vector<std::unique_ptr<DefStmt>> methods;
        ClassStmt(std::string n, std::string p, SourceLocation l)
            : Stmt(StmtKind::Class, l), name(std::move(n)), parentName(std::move(p)) {
        }
    };

    struct ForStmt : Stmt {
        std::string targetName;
        ExprPtr     iterable;
        Block       body;
        ForStmt(std::string t, ExprPtr it, Block b, SourceLocation l)
            : Stmt(StmtKind::For, l),
            targetName(std::move(t)), iterable(std::move(it)), body(std::move(b)) {
        }
    };

    struct ExceptClause {
        ExprPtr     exceptionType;
        std::string varName;
        Block       body;
    };
    struct TryStmt : Stmt {
        Block                     tryBody;
        std::vector<ExceptClause> handlers;
        std::optional<Block>      finallyBody;
        TryStmt(Block tb, SourceLocation l)
            : Stmt(StmtKind::Try, l), tryBody(std::move(tb)) {
        }
    };
    struct RaiseStmt : Stmt {
        ExprPtr exception;
        RaiseStmt(ExprPtr e, SourceLocation l)
            : Stmt(StmtKind::Raise, l), exception(std::move(e)) {
        }
    };

    struct ImportStmt : Stmt {
        std::string moduleName;
        std::string alias;
        ImportStmt(std::string m, std::string a, SourceLocation l)
            : Stmt(StmtKind::Import, l),
            moduleName(std::move(m)), alias(std::move(a)) {
        }
    };

    struct ImportItem {
        std::string name;
        std::string alias;
    };
    struct FromImportStmt : Stmt {
        std::string             moduleName;
        std::vector<ImportItem> items;
        FromImportStmt(std::string m, std::vector<ImportItem> i, SourceLocation l)
            : Stmt(StmtKind::FromImport, l),
            moduleName(std::move(m)), items(std::move(i)) {
        }
    };

    struct PassStmt : Stmt { PassStmt(SourceLocation l) : Stmt(StmtKind::Pass, l) {} };
    struct BreakStmt : Stmt { BreakStmt(SourceLocation l) : Stmt(StmtKind::Break, l) {} };
    struct ContinueStmt : Stmt { ContinueStmt(SourceLocation l) : Stmt(StmtKind::Continue, l) {} };

    struct YieldStmt : Stmt {
        ExprPtr value;
        YieldStmt(ExprPtr v, SourceLocation l)
            : Stmt(StmtKind::Yield, l), value(std::move(v)) {
        }
    };

    struct ConstStmt : Stmt {
        std::string name;
        ExprPtr     type;
        ExprPtr     value;
        ConstStmt(std::string n, ExprPtr t, ExprPtr v, SourceLocation l)
            : Stmt(StmtKind::Const, l), name(std::move(n)),
            type(std::move(t)), value(std::move(v)) {
        }
    };

    struct EnumItem {
        std::string name;
        ExprPtr     value;
    };
    struct EnumStmt : Stmt {
        std::string           name;
        std::vector<EnumItem> items;
        EnumStmt(std::string n, SourceLocation l)
            : Stmt(StmtKind::Enum, l), name(std::move(n)) {
        }
    };

    // Phase 14.0b: inline statement block.  Used by the tuple-unpack desugar
    // so that names bound inside stay visible in the enclosing scope (matches
    // tree-walk and VM behavior; no new environment is created).
    struct BlockStmt : Stmt {
        Block body;
        BlockStmt(Block b, SourceLocation l)
            : Stmt(StmtKind::Block, l), body(std::move(b)) {
        }
    };
    // Phase 15.0: extern "C" declarations.
    struct ExternFnDecl {
        std::string name;
        std::vector<Param> params;
        ExprPtr returnType;
        SourceLocation loc;
    };

    struct ExternBlockStmt : Stmt {
        std::string abi;              // "C" for now
        std::vector<ExternFnDecl> funcs;
        ExternBlockStmt(std::string a, SourceLocation l)
            : Stmt(StmtKind::Extern, l), abi(std::move(a)) {
        }
    };
    const char* binOpName(BinOp op);
    const char* unOpName(UnOp  op);
    void printProgram(const Block& program);

} // namespace vayu