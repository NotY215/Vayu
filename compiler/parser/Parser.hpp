#pragma once
#include "ast/Ast.hpp"
#include "lexer/Token.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace vayu {

    class ParseError : public std::runtime_error {
    public:
        SourceLocation loc;
        ParseError(std::string msg, SourceLocation l)
            : std::runtime_error(std::move(msg)), loc(l) {}
    };

    class Parser {
    public:
        explicit Parser(std::vector<Token> tokens);
        Block parseProgram();

    private:
        std::vector<Token> tokens_;
        size_t             pos_ = 0;
        int                matchCounter_ = 0;
        std::unordered_set<std::string> namespaceNames_;

        const Token& peek(int ahead = 0) const;
        const Token& previous() const;
        bool         isAtEnd() const;
        bool         check(TokenType t) const;
        bool         match(TokenType t);
        const Token& advance();
        const Token& expect(TokenType t, const char* what);
        void         skipNewlines();
        bool         isNamespaceAhead() const;

        StmtPtr  parseStatement();
        Block    parseBlock();
        Block    parseNamespaceBody();
        std::unique_ptr<DefStmt> parseDef();
        StmtPtr  parseIf();
        StmtPtr  parseWhile();
        StmtPtr  parseFor();
        StmtPtr  parseReturn();
        StmtPtr  parseStruct();
        StmtPtr  parseClass();
        StmtPtr  parseTry();
        StmtPtr  parseRaise();
        StmtPtr  parseImport();
        StmtPtr  parseFromImport();
        StmtPtr  parseAnnotatedAssign();
        StmtPtr  parseExprOrAssign();
        StmtPtr  parseConst();
        StmtPtr  parseEnum();
        StmtPtr  parseMatch();
        StmtPtr  parseWith();
        StmtPtr  parseYield();
        StmtPtr  buildCompoundAssign(ExprPtr target, BinOp op,
            ExprPtr rhs, SourceLocation loc);
        Param    parseParam();
        FieldDef parseFieldDef();
        bool     consumeVisibility(Visibility& out);

        ExprPtr  parseExpression();
        ExprPtr  parseLambda();
        ExprPtr  parseBinary(int minPrec);
        ExprPtr  parseUnary();
        ExprPtr  parsePostfix();
        ExprPtr  parsePrimary();
        ExprPtr  parseListLit();
        ExprPtr  parseMapLit();
        CallArg  parseCallArg();
        ExprPtr  parseTypeExpr();
    };

} // namespace vayu