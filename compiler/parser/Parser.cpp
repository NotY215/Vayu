#include "Parser.hpp"
#include <cstdlib>
#include <functional>

namespace vayu {

    // Precedence, from loosest to tightest:
    //   1  or
    //   2  and
    //   3  ==  !=  <  >  <=  >=  in  is
    //   4  |
    //   5  ^
    //   6  &
    //   7  <<  >>
    //   8  +  -
    //   9  *  /  //  %
    //  11  **
    static int binPrec(TokenType t) {
        switch (t) {
        case TokenType::Or: return 1;
        case TokenType::And: return 2;
        case TokenType::Eq: case TokenType::NotEq:
        case TokenType::Lt: case TokenType::Gt:
        case TokenType::LtEq: case TokenType::GtEq:
        case TokenType::In: case TokenType::Is: return 3;
        case TokenType::Pipe: return 4;
        case TokenType::Caret: return 5;
        case TokenType::Amp: return 6;
        case TokenType::Shl: case TokenType::Shr: return 7;
        case TokenType::Plus: case TokenType::Minus: return 8;
        case TokenType::Star: case TokenType::Slash:
        case TokenType::SlashSlash: case TokenType::Percent: return 9;
        case TokenType::StarStar: return 11;
        default: return -1;
        }
    }
    static BinOp tokenToBinOp(TokenType t) {
        switch (t) {
        case TokenType::Plus: return BinOp::Add;
        case TokenType::Minus: return BinOp::Sub;
        case TokenType::Star: return BinOp::Mul;
        case TokenType::Slash: return BinOp::Div;
        case TokenType::SlashSlash: return BinOp::FloorDiv;
        case TokenType::Percent: return BinOp::Mod;
        case TokenType::StarStar: return BinOp::Pow;
        case TokenType::Eq: return BinOp::Eq;
        case TokenType::NotEq: return BinOp::NotEq;
        case TokenType::Lt: return BinOp::Lt;
        case TokenType::Gt: return BinOp::Gt;
        case TokenType::LtEq: return BinOp::LtEq;
        case TokenType::GtEq: return BinOp::GtEq;
        case TokenType::And: return BinOp::And;
        case TokenType::Or: return BinOp::Or;
        case TokenType::In: return BinOp::In;
        case TokenType::Is: return BinOp::Is;
        case TokenType::Amp: return BinOp::BAnd;
        case TokenType::Pipe: return BinOp::BOr;
        case TokenType::Caret: return BinOp::BXor;
        case TokenType::Shl: return BinOp::Shl;
        case TokenType::Shr: return BinOp::Shr;
        default: return BinOp::Add;
        }
    }

    // Parse an integer literal with optional 0b/0o/0x prefix.
    static long long parseIntLiteralText(const std::string& s, SourceLocation loc) {
        try {
            if (s.size() >= 2 && s[0] == '0') {
                char c = s[1];
                if (c == 'b' || c == 'B') return std::stoll(s.substr(2), nullptr, 2);
                if (c == 'o' || c == 'O') return std::stoll(s.substr(2), nullptr, 8);
                if (c == 'x' || c == 'X') return std::stoll(s.substr(2), nullptr, 16);
            }
            return std::stoll(s, nullptr, 10);
        }
        catch (...) {
            throw ParseError("integer literal out of range", loc);
        }
    }

    Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    const Token& Parser::peek(int ahead) const {
        size_t i = pos_ + static_cast<size_t>(ahead);
        return i >= tokens_.size() ? tokens_.back() : tokens_[i];
    }
    const Token& Parser::previous() const { return tokens_[pos_ - 1]; }
    bool Parser::isAtEnd() const { return peek().type == TokenType::EndOfFile; }
    bool Parser::check(TokenType t) const { return peek().type == t; }
    bool Parser::match(TokenType t) {
        if (check(t)) { advance(); return true; }
        return false;
    }
    const Token& Parser::advance() {
        if (!isAtEnd()) ++pos_;
        return tokens_[pos_ - 1];
    }
    const Token& Parser::expect(TokenType t, const char* what) {
        if (check(t)) return advance();
        throw ParseError(std::string("expected ") + what + ", found '" +
            peek().lexeme + "'", peek().location);
    }
    void Parser::skipNewlines() { while (check(TokenType::Newline)) advance(); }

    // =========================================================================
    // Phase 11.1h — namespace detection & splicing
    // =========================================================================

    bool Parser::isNamespaceAhead() const {
        return check(TokenType::Identifier) && peek().lexeme == "namespace" &&
            peek(1).type == TokenType::Identifier;
    }

    Block Parser::parseNamespaceBody() {
        advance();   // 'namespace'
        Token nameTok = expect(TokenType::Identifier, "namespace name");
        std::string nsName = nameTok.lexeme;

        namespaceNames_.insert(nsName);

        expect(TokenType::Colon, "':' after namespace name");
        Block body = parseBlock();

        // Prefix every top-level named definition with "Ns.".
        for (auto& st : body.stmts) {
            switch (st->kind) {
            case StmtKind::Def:
                static_cast<DefStmt*>(st.get())->name =
                    nsName + "." + static_cast<DefStmt*>(st.get())->name;
                break;
            case StmtKind::Const:
                static_cast<ConstStmt*>(st.get())->name =
                    nsName + "." + static_cast<ConstStmt*>(st.get())->name;
                break;
            case StmtKind::Enum:
                static_cast<EnumStmt*>(st.get())->name =
                    nsName + "." + static_cast<EnumStmt*>(st.get())->name;
                break;
            case StmtKind::Class:
                static_cast<ClassStmt*>(st.get())->name =
                    nsName + "." + static_cast<ClassStmt*>(st.get())->name;
                break;
            default: break;
            }
        }
        return body;
    }

    Block Parser::parseProgram() {
        Block p;
        for (;;) {
            skipNewlines();
            if (isAtEnd()) break;
            if (isNamespaceAhead()) {
                Block nsBody = parseNamespaceBody();
                for (auto& st : nsBody.stmts) p.stmts.push_back(std::move(st));
                continue;
            }
            p.stmts.push_back(parseStatement());
        }
        return p;
    }

    Block Parser::parseBlock() {
        expect(TokenType::Newline, "newline after ':'");
        expect(TokenType::Indent, "indented block");

        Block b;
        std::vector<ExprPtr> defers;
        bool sawNonDefer = false;

        for (;;) {
            skipNewlines();
            if (isAtEnd() || check(TokenType::Dedent)) break;

            // Phase 11.1h: namespace body spliced inline.
            if (isNamespaceAhead()) {
                Block nsBody = parseNamespaceBody();
                for (auto& st : nsBody.stmts) {
                    b.stmts.push_back(std::move(st));
                    sawNonDefer = true;
                }
                continue;
            }

            // Phase 11.1g: `defer expr` — soft keyword, block-top only.
            if (check(TokenType::Identifier) && peek().lexeme == "defer" &&
                peek(1).type != TokenType::Assign &&
                peek(1).type != TokenType::PlusAssign &&
                peek(1).type != TokenType::MinusAssign &&
                peek(1).type != TokenType::StarAssign &&
                peek(1).type != TokenType::SlashAssign &&
                peek(1).type != TokenType::Dot &&
                peek(1).type != TokenType::LBracket &&
                peek(1).type != TokenType::Colon) {
                if (sawNonDefer)
                    throw ParseError(
                        "'defer' must appear at the top of a block",
                        peek().location);
                advance();
                ExprPtr e = parseExpression();
                defers.push_back(std::move(e));
                continue;
            }
            sawNonDefer = true;
            b.stmts.push_back(parseStatement());
        }
        match(TokenType::Dedent);

        if (!defers.empty()) {
            // Wrap the body with nested try/finally so that defers run LIFO.
            Block result = std::move(b);
            for (int i = (int)defers.size() - 1; i >= 0; --i) {
                Block fin;
                fin.stmts.push_back(std::make_unique<ExprStmt>(
                    std::move(defers[(size_t)i]), SourceLocation{}));
                auto ts = std::make_unique<TryStmt>(
                    std::move(result), SourceLocation{});
                ts->finallyBody = std::move(fin);
                Block wrapper;
                wrapper.stmts.push_back(std::move(ts));
                result = std::move(wrapper);
            }
            return result;
        }
        return b;
    }

    StmtPtr Parser::parseStatement() {
        if (check(TokenType::Def))    return parseDef();
        if (check(TokenType::Struct)) return parseStruct();
        if (check(TokenType::Class))  return parseClass();
        if (check(TokenType::If))     return parseIf();
        if (check(TokenType::While))  return parseWhile();
        if (check(TokenType::For))    return parseFor();
        if (check(TokenType::Return)) return parseReturn();
        if (check(TokenType::Try))    return parseTry();
        if (check(TokenType::Raise))  return parseRaise();
        if (check(TokenType::Import)) return parseImport();
        if (check(TokenType::From))   return parseFromImport();
        if (check(TokenType::Const))  return parseConst();
        if (check(TokenType::Enum))   return parseEnum();
        if (check(TokenType::With))   return parseWith();
        if (check(TokenType::Yield))  return parseYield();
        if (check(TokenType::Extern)) return parseExtern();

        // Soft keyword `match`.  Try to parse as a match statement first,
        // and fall back to a normal expression statement (e.g. `match(x)`)
        // if it doesn't work out.
        if (check(TokenType::Identifier) && peek().lexeme == "match" &&
            peek(1).type != TokenType::Assign &&
            peek(1).type != TokenType::PlusAssign &&
            peek(1).type != TokenType::MinusAssign &&
            peek(1).type != TokenType::StarAssign &&
            peek(1).type != TokenType::SlashAssign &&
            peek(1).type != TokenType::Dot) {
            size_t saved = pos_;
            int savedMatch = matchCounter_;
            try {
                return parseMatch();
            }
            catch (const ParseError&) {
                pos_ = saved;
                matchCounter_ = savedMatch;
            }
        }

        if (check(TokenType::Pass)) {
            Token t = advance();
            return std::make_unique<PassStmt>(t.location);
        }
        if (check(TokenType::Break)) {
            Token t = advance();
            return std::make_unique<BreakStmt>(t.location);
        }
        if (check(TokenType::Continue)) {
            Token t = advance();
            return std::make_unique<ContinueStmt>(t.location);
        }

        if (check(TokenType::Identifier) && peek(1).type == TokenType::Colon)
            return parseAnnotatedAssign();

        // Phase 14.0b: `a, b, c = expr` — desugared to
        //   __unpack_N = expr
        //   a = __unpack_N[0]
        //   b = __unpack_N[1]
        //   ...
        // wrapped in a trivially-true `if` so parseStatement can return
        // a single StmtPtr.
        if (check(TokenType::Identifier) &&
            peek(1).type == TokenType::Comma) {
            size_t save = pos_;
            std::vector<std::string> names;
            names.push_back(advance().lexeme);
            bool ok = true;
            while (match(TokenType::Comma)) {
                if (!check(TokenType::Identifier)) { ok = false; break; }
                names.push_back(advance().lexeme);
            }
            if (!ok || names.size() < 2 || !check(TokenType::Assign)) {
                pos_ = save;
            }
            else {
                Token eqTok = advance();
                ExprPtr value = parseExpression();
                std::string tmpName = "__unpack_" +
                    std::to_string(matchCounter_++);
                Block blk;
                blk.stmts.push_back(std::make_unique<AssignStmt>(
                    std::make_unique<NameRefExpr>(tmpName, eqTok.location),
                    std::move(value),
                    eqTok.location));
                for (size_t i = 0; i < names.size(); ++i) {
                    blk.stmts.push_back(std::make_unique<AssignStmt>(
                        std::make_unique<NameRefExpr>(names[i], eqTok.location),
                        std::make_unique<IndexExpr>(
                            std::make_unique<NameRefExpr>(tmpName,
                                eqTok.location),
                            std::make_unique<IntLitExpr>((long long)i,
                                std::to_string(i), eqTok.location),
                            eqTok.location),
                        eqTok.location));
                }
                return std::make_unique<BlockStmt>(
                    std::move(blk), eqTok.location);
            }
        }

        return parseExprOrAssign();
    }

    // =========================================================================
    // Phase 11.1e — match / case
    // =========================================================================

    StmtPtr Parser::parseMatch() {
        Token mTok = advance();
        ExprPtr subject = parseExpression();
        expect(TokenType::Colon, "':' after match subject");
        expect(TokenType::Newline, "newline after ':'");
        expect(TokenType::Indent, "indented match body");

        struct RawCase {
            ExprPtr pattern;
            std::string binder;
            std::vector<std::string> tupleNames;
            ExprPtr guard;
            Block body;
            SourceLocation loc;
        };
        std::vector<RawCase> cases;

        for (;;) {
            skipNewlines();
            if (isAtEnd() || check(TokenType::Dedent)) break;

            if (!(check(TokenType::Identifier) && peek().lexeme == "case"))
                throw ParseError("expected 'case' in match body",
                    peek().location);
            Token cTok = advance();

            RawCase rc;
            rc.loc = cTok.location;

            if (check(TokenType::Identifier) && peek().lexeme == "_" &&
                (peek(1).type == TokenType::Colon ||
                    peek(1).type == TokenType::If)) {
                advance();
            }
            else if (check(TokenType::Identifier) &&
                (peek(1).type == TokenType::Colon ||
                    peek(1).type == TokenType::If)) {
                Token n = advance();
                rc.binder = n.lexeme;
            }
            else if (check(TokenType::LParen)) {
                advance();
                if (!check(TokenType::RParen)) {
                    for (;;) {
                        Token n = expect(TokenType::Identifier,
                            "tuple pattern name");
                        rc.tupleNames.push_back(n.lexeme);
                        if (!match(TokenType::Comma)) break;
                        if (check(TokenType::RParen)) break;
                    }
                }
                expect(TokenType::RParen, "')' to close tuple pattern");
            }
            else {
                rc.pattern = parseExpression();
            }

            if (match(TokenType::If)) rc.guard = parseExpression();

            expect(TokenType::Colon, "':' after case pattern");
            rc.body = parseBlock();
            cases.push_back(std::move(rc));
        }
        match(TokenType::Dedent);

        if (cases.empty())
            throw ParseError("match requires at least one case", mTok.location);

        std::string subjName = "__match_subject_" +
            std::to_string(matchCounter_++);
        std::string doneName = "__match_done_" +
            std::to_string(matchCounter_++);

        Block result;
        result.stmts.push_back(std::make_unique<AssignStmt>(
            std::make_unique<NameRefExpr>(subjName, mTok.location),
            std::move(subject),
            mTok.location));
        result.stmts.push_back(std::make_unique<AssignStmt>(
            std::make_unique<NameRefExpr>(doneName, mTok.location),
            std::make_unique<BoolLitExpr>(false, mTok.location),
            mTok.location));

        for (auto& rc : cases) {
            ExprPtr cond = std::make_unique<UnaryExpr>(
                UnOp::Not,
                std::make_unique<NameRefExpr>(doneName, rc.loc),
                rc.loc);
            if (rc.pattern) {
                ExprPtr eq = std::make_unique<BinaryExpr>(
                    BinOp::Eq,
                    std::make_unique<NameRefExpr>(subjName, rc.loc),
                    std::move(rc.pattern),
                    rc.loc);
                cond = std::make_unique<BinaryExpr>(
                    BinOp::And,
                    std::move(cond),
                    std::move(eq),
                    rc.loc);
            }

            Block caseBody;
            if (!rc.binder.empty()) {
                caseBody.stmts.push_back(std::make_unique<AssignStmt>(
                    std::make_unique<NameRefExpr>(rc.binder, rc.loc),
                    std::make_unique<NameRefExpr>(subjName, rc.loc),
                    rc.loc));
            }
            for (size_t i = 0; i < rc.tupleNames.size(); ++i) {
                caseBody.stmts.push_back(std::make_unique<AssignStmt>(
                    std::make_unique<NameRefExpr>(rc.tupleNames[i], rc.loc),
                    std::make_unique<IndexExpr>(
                        std::make_unique<NameRefExpr>(subjName, rc.loc),
                        std::make_unique<IntLitExpr>((long long)i,
                            std::to_string(i), rc.loc),
                        rc.loc),
                    rc.loc));
            }

            Block marked;
            marked.stmts.push_back(std::make_unique<AssignStmt>(
                std::make_unique<NameRefExpr>(doneName, rc.loc),
                std::make_unique<BoolLitExpr>(true, rc.loc),
                rc.loc));
            for (auto& st : rc.body.stmts) marked.stmts.push_back(std::move(st));

            if (rc.guard) {
                caseBody.stmts.push_back(std::make_unique<IfStmt>(
                    std::move(rc.guard), std::move(marked), rc.loc));
            }
            else {
                for (auto& st : marked.stmts)
                    caseBody.stmts.push_back(std::move(st));
            }

            result.stmts.push_back(std::make_unique<IfStmt>(
                std::move(cond), std::move(caseBody), rc.loc));
        }

        return std::make_unique<IfStmt>(
            std::make_unique<BoolLitExpr>(true, mTok.location),
            std::move(result),
            mTok.location);
    }

    // =========================================================================
    // Phase 11.1f — with statement
    // =========================================================================

    StmtPtr Parser::parseWith() {
        Token wTok = advance();   // 'with'
        ExprPtr res = parseExpression();
        std::string name;
        if (match(TokenType::As)) {
            Token n = expect(TokenType::Identifier, "name after 'as'");
            name = n.lexeme;
        }
        expect(TokenType::Colon, "':' after with");
        Block body = parseBlock();

        std::string tmp = "__with_val_" + std::to_string(matchCounter_++);

        Block result;
        result.stmts.push_back(std::make_unique<AssignStmt>(
            std::make_unique<NameRefExpr>(tmp, wTok.location),
            std::move(res),
            wTok.location));

        if (!name.empty()) {
            std::vector<CallArg> noArgs;
            auto enterCall = std::make_unique<CallExpr>(
                std::make_unique<AttrExpr>(
                    std::make_unique<NameRefExpr>(tmp, wTok.location),
                    "__enter__",
                    wTok.location),
                std::move(noArgs),
                wTok.location);
            result.stmts.push_back(std::make_unique<AssignStmt>(
                std::make_unique<NameRefExpr>(name, wTok.location),
                std::move(enterCall),
                wTok.location));
        }

        auto ts = std::make_unique<TryStmt>(std::move(body), wTok.location);
        Block fin;
        {
            std::vector<CallArg> noArgs;
            fin.stmts.push_back(std::make_unique<ExprStmt>(
                std::make_unique<CallExpr>(
                    std::make_unique<AttrExpr>(
                        std::make_unique<NameRefExpr>(tmp, wTok.location),
                        "__exit__",
                        wTok.location),
                    std::move(noArgs),
                    wTok.location),
                wTok.location));
        }
        ts->finallyBody = std::move(fin);
        result.stmts.push_back(std::move(ts));

        return std::make_unique<IfStmt>(
            std::make_unique<BoolLitExpr>(true, wTok.location),
            std::move(result),
            wTok.location);
    }

    // =========================================================================
    // Phase 11.1k1 — yield
    // =========================================================================

    StmtPtr Parser::parseYield() {
        Token yTok = advance();
        ExprPtr value = nullptr;
        if (!check(TokenType::Newline) && !check(TokenType::Dedent) &&
            !isAtEnd())
            value = parseExpression();
        return std::make_unique<YieldStmt>(std::move(value), yTok.location);
    }

    // =========================================================================
    // Phase 11.1i — compound assign desugar
    // =========================================================================

    StmtPtr Parser::buildCompoundAssign(ExprPtr target, BinOp op,
        ExprPtr rhs, SourceLocation loc) {
        if (target->kind == ExprKind::NameRef) {
            const auto* n = static_cast<const NameRefExpr*>(target.get());
            std::string name = n->name;
            auto read = std::make_unique<NameRefExpr>(name, loc);
            auto b = std::make_unique<BinaryExpr>(op, std::move(read),
                std::move(rhs), loc);
            auto write = std::make_unique<NameRefExpr>(name, loc);
            return std::make_unique<AssignStmt>(std::move(write),
                std::move(b), loc);
        }
        if (target->kind == ExprKind::Attr) {
            auto* a = static_cast<const AttrExpr*>(target.get());
            if (a->target->kind == ExprKind::NameRef) {
                const auto* base =
                    static_cast<const NameRefExpr*>(a->target.get());
                std::string baseName = base->name;
                std::string attrName = a->name;
                auto read = std::make_unique<AttrExpr>(
                    std::make_unique<NameRefExpr>(baseName, loc),
                    attrName, loc);
                auto b = std::make_unique<BinaryExpr>(op, std::move(read),
                    std::move(rhs), loc);
                auto write = std::make_unique<AttrExpr>(
                    std::make_unique<NameRefExpr>(baseName, loc),
                    attrName, loc);
                return std::make_unique<AssignStmt>(std::move(write),
                    std::move(b), loc);
            }
        }
        if (target->kind == ExprKind::Index) {
            auto* ix = static_cast<const IndexExpr*>(target.get());
            if (ix->target->kind == ExprKind::NameRef &&
                ix->index->kind == ExprKind::NameRef) {
                const auto* base =
                    static_cast<const NameRefExpr*>(ix->target.get());
                const auto* idx =
                    static_cast<const NameRefExpr*>(ix->index.get());
                std::string baseName = base->name;
                std::string idxName = idx->name;
                auto read = std::make_unique<IndexExpr>(
                    std::make_unique<NameRefExpr>(baseName, loc),
                    std::make_unique<NameRefExpr>(idxName, loc), loc);
                auto b = std::make_unique<BinaryExpr>(op, std::move(read),
                    std::move(rhs), loc);
                auto write = std::make_unique<IndexExpr>(
                    std::make_unique<NameRefExpr>(baseName, loc),
                    std::make_unique<NameRefExpr>(idxName, loc), loc);
                return std::make_unique<AssignStmt>(std::move(write),
                    std::move(b), loc);
            }
        }
        throw ParseError(
            "compound assign requires a simple name, `obj.field`, or `obj[i]` "
            "where obj and i are plain names", loc);
    }

    StmtPtr Parser::parseExprOrAssign() {
        SourceLocation start = peek().location;
        ExprPtr expr = parseExpression();

        BinOp compoundOp = BinOp::Add;
        bool isCompound = false;
        if (match(TokenType::PlusAssign)) { compoundOp = BinOp::Add; isCompound = true; }
        else if (match(TokenType::MinusAssign)) { compoundOp = BinOp::Sub; isCompound = true; }
        else if (match(TokenType::StarAssign)) { compoundOp = BinOp::Mul; isCompound = true; }
        else if (match(TokenType::SlashAssign)) { compoundOp = BinOp::Div; isCompound = true; }
        else if (match(TokenType::PercentAssign)) { compoundOp = BinOp::Mod; isCompound = true; }
        else if (match(TokenType::StarStarAssign)) { compoundOp = BinOp::Pow; isCompound = true; }
        else if (match(TokenType::SlashSlashAssign)) { compoundOp = BinOp::FloorDiv; isCompound = true; }
        else if (match(TokenType::AmpAssign)) { compoundOp = BinOp::BAnd; isCompound = true; }
        else if (match(TokenType::PipeAssign)) { compoundOp = BinOp::BOr; isCompound = true; }
        else if (match(TokenType::CaretAssign)) { compoundOp = BinOp::BXor; isCompound = true; }
        else if (match(TokenType::ShlAssign)) { compoundOp = BinOp::Shl; isCompound = true; }
        else if (match(TokenType::ShrAssign)) { compoundOp = BinOp::Shr; isCompound = true; }

        if (isCompound) {
            ExprPtr rhs = parseExpression();
            return buildCompoundAssign(std::move(expr), compoundOp,
                std::move(rhs), start);
        }

        if (match(TokenType::Assign)) {
            ExprPtr value = parseExpression();
            return std::make_unique<AssignStmt>(std::move(expr),
                std::move(value), start);
        }
        return std::make_unique<ExprStmt>(std::move(expr), start);
    }

    StmtPtr Parser::parseAnnotatedAssign() {
        Token name = advance();
        advance();   // ':'
        ExprPtr type = parseTypeExpr();
        ExprPtr value = nullptr;
        if (match(TokenType::Assign)) value = parseExpression();
        return std::make_unique<AnnotAssignStmt>(name.lexeme, std::move(type),
            std::move(value), name.location);
    }

    StmtPtr Parser::parseReturn() {
        Token t = advance();
        ExprPtr value = nullptr;
        if (!check(TokenType::Newline) && !check(TokenType::Dedent) &&
            !isAtEnd())
            value = parseExpression();
        return std::make_unique<ReturnStmt>(std::move(value), t.location);
    }

    StmtPtr Parser::parseRaise() {
        Token t = advance();
        ExprPtr exc = nullptr;
        if (!check(TokenType::Newline) && !check(TokenType::Dedent) &&
            !isAtEnd())
            exc = parseExpression();
        return std::make_unique<RaiseStmt>(std::move(exc), t.location);
    }

    StmtPtr Parser::parseConst() {
        Token cTok = advance();
        Token name = expect(TokenType::Identifier, "constant name");
        ExprPtr type = nullptr;
        if (match(TokenType::Colon)) type = parseTypeExpr();
        expect(TokenType::Assign, "'=' in const declaration");
        ExprPtr value = parseExpression();
        return std::make_unique<ConstStmt>(name.lexeme, std::move(type),
            std::move(value), cTok.location);
    }

    StmtPtr Parser::parseEnum() {
        Token eTok = advance();
        Token name = expect(TokenType::Identifier, "enum name");
        expect(TokenType::Colon, "':' after enum name");
        expect(TokenType::Newline, "newline after ':'");
        expect(TokenType::Indent, "indented enum body");

        auto stmt = std::make_unique<EnumStmt>(name.lexeme, eTok.location);
        long long nextVal = 0;
        bool autoIncrementOK = true;

        for (;;) {
            skipNewlines();
            if (isAtEnd() || check(TokenType::Dedent)) break;

            Token item = expect(TokenType::Identifier, "enum item name");
            EnumItem it;
            it.name = item.lexeme;
            it.value = nullptr;

            if (match(TokenType::Assign)) {
                it.value = parseExpression();
                if (it.value->kind == ExprKind::IntLit) {
                    nextVal = static_cast<const IntLitExpr*>(
                        it.value.get())->value + 1;
                    autoIncrementOK = true;
                }
                else {
                    autoIncrementOK = false;
                }
            }
            else {
                if (!autoIncrementOK)
                    throw ParseError(
                        "enum item '" + it.name +
                        "' must have an explicit value after a non-literal item",
                        item.location);
                it.value = std::make_unique<IntLitExpr>(
                    nextVal, std::to_string(nextVal), item.location);
                nextVal++;
            }

            stmt->items.push_back(std::move(it));
        }
        match(TokenType::Dedent);

        if (stmt->items.empty())
            throw ParseError("enum '" + name.lexeme + "' has no items",
                eTok.location);

        for (size_t i = 0; i < stmt->items.size(); ++i)
            for (size_t j = i + 1; j < stmt->items.size(); ++j)
                if (stmt->items[i].name == stmt->items[j].name)
                    throw ParseError("enum '" + name.lexeme +
                        "' declares '" + stmt->items[i].name + "' twice",
                        eTok.location);

        return stmt;
    }

    StmtPtr Parser::parseTry() {
        Token tryTok = advance();
        expect(TokenType::Colon, "':' after try");
        Block tryBody = parseBlock();
        auto stmt = std::make_unique<TryStmt>(std::move(tryBody),
            tryTok.location);
        bool sawExcept = false;
        while (check(TokenType::Except)) {
            sawExcept = true;
            advance();
            ExceptClause clause;
            if (!check(TokenType::Colon)) {
                clause.exceptionType = parseExpression();
                if (match(TokenType::As)) {
                    Token var = expect(TokenType::Identifier,
                        "variable name after 'as'");
                    clause.varName = var.lexeme;
                }
            }
            expect(TokenType::Colon, "':' after except");
            clause.body = parseBlock();
            stmt->handlers.push_back(std::move(clause));
        }
        if (match(TokenType::Finally)) {
            expect(TokenType::Colon, "':' after finally");
            stmt->finallyBody = parseBlock();
        }
        if (!sawExcept && !stmt->finallyBody)
            throw ParseError("try block must have at least one except or a finally",
                tryTok.location);
        return stmt;
    }

    StmtPtr Parser::parseIf() {
        Token ifTok = advance();
        ExprPtr cond = parseExpression();
        expect(TokenType::Colon, "':' after if condition");
        Block thenBody = parseBlock();
        auto stmt = std::make_unique<IfStmt>(std::move(cond),
            std::move(thenBody), ifTok.location);
        while (check(TokenType::Elif)) {
            advance();
            ExprPtr econd = parseExpression();
            expect(TokenType::Colon, "':' after elif condition");
            Block ebody = parseBlock();
            stmt->elifs.push_back(ElifClause{ std::move(econd),
                                              std::move(ebody) });
        }
        if (check(TokenType::Else)) {
            advance();
            expect(TokenType::Colon, "':' after else");
            stmt->elseBody = parseBlock();
        }
        return stmt;
    }

    StmtPtr Parser::parseWhile() {
        Token wTok = advance();
        ExprPtr cond = parseExpression();
        expect(TokenType::Colon, "':' after while condition");
        Block body = parseBlock();
        return std::make_unique<WhileStmt>(std::move(cond), std::move(body),
            wTok.location);
    }

    StmtPtr Parser::parseFor() {
        Token fTok = advance();
        std::vector<std::string> names;
        if (check(TokenType::LParen)) {
            advance();
            names.push_back(expect(TokenType::Identifier,
                "loop variable").lexeme);
            while (match(TokenType::Comma)) {
                if (check(TokenType::RParen)) break;
                names.push_back(expect(TokenType::Identifier,
                    "loop variable").lexeme);
            }
            expect(TokenType::RParen, "')' after loop variables");
        }
        else {
            names.push_back(expect(TokenType::Identifier,
                "loop variable").lexeme);
        }
        expect(TokenType::In, "'in' after loop variable");
        ExprPtr iterable = parseExpression();
        expect(TokenType::Colon, "':' after iterable");
        Block body = parseBlock();

        if (names.size() == 1) {
            return std::make_unique<ForStmt>(names[0], std::move(iterable),
                std::move(body), fTok.location);
        }

        // Phase 14.0b: `for (k, v) in xs:` desugars to
        //   for __for_item_N in xs:
        //       k = __for_item_N[0]
        //       v = __for_item_N[1]
        //       <original body>
        std::string tmpName = "__for_item_" +
            std::to_string(matchCounter_++);
        Block newBody;
        for (size_t i = 0; i < names.size(); ++i) {
            newBody.stmts.push_back(std::make_unique<AssignStmt>(
                std::make_unique<NameRefExpr>(names[i], fTok.location),
                std::make_unique<IndexExpr>(
                    std::make_unique<NameRefExpr>(tmpName, fTok.location),
                    std::make_unique<IntLitExpr>((long long)i,
                        std::to_string(i), fTok.location),
                    fTok.location),
                fTok.location));
        }
        for (auto& st : body.stmts) newBody.stmts.push_back(std::move(st));

        return std::make_unique<ForStmt>(tmpName, std::move(iterable),
            std::move(newBody), fTok.location);
    }

    Param Parser::parseParam() {
        Token name = expect(TokenType::Identifier, "parameter name");
        Param p; p.name = name.lexeme;
        if (match(TokenType::Colon)) p.type = parseTypeExpr();
        return p;
    }

    std::unique_ptr<DefStmt> Parser::parseDef() {
        Token defTok = advance();
        Token name = expect(TokenType::Identifier, "function name");

        // Phase 11.2: optional type parameter list `<T, U: Constraint, ...>`.
        std::vector<std::string> typeParams;
        std::vector<std::string> typeParamConstraints;
        if (match(TokenType::Lt)) {
            for (;;) {
                Token tp = expect(TokenType::Identifier,
                    "type parameter name");
                std::string constraint;
                if (match(TokenType::Colon)) {
                    Token c = expect(TokenType::Identifier,
                        "constraint name");
                    constraint = c.lexeme;
                }
                typeParams.push_back(tp.lexeme);
                typeParamConstraints.push_back(std::move(constraint));
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::Gt, "'>' to close type parameter list");
        }

        expect(TokenType::LParen, "'(' after function name");
        std::vector<Param> params;
        if (!check(TokenType::RParen)) {
            params.push_back(parseParam());
            while (match(TokenType::Comma)) {
                if (check(TokenType::RParen)) break;
                params.push_back(parseParam());
            }
        }
        expect(TokenType::RParen, "')' to close parameter list");
        ExprPtr retType = nullptr;
        if (match(TokenType::Arrow)) retType = parseTypeExpr();
        expect(TokenType::Colon, "':' before function body");
        Block body = parseBlock();

        auto def = std::make_unique<DefStmt>(name.lexeme, std::move(params),
            std::move(retType), std::move(body),
            defTok.location);
        def->typeParams = std::move(typeParams);
        def->typeParamConstraints = std::move(typeParamConstraints);

        // Phase 11.1k1: mark generator functions by scanning the body for
        // a top-level `yield`.
        std::function<bool(const Block&)> blockHasYield =
            [&](const Block& b) -> bool {
            for (auto& s : b.stmts) {
                switch (s->kind) {
                case StmtKind::Yield: return true;
                case StmtKind::If: {
                    auto* n = static_cast<const IfStmt*>(s.get());
                    if (blockHasYield(n->thenBody)) return true;
                    for (auto& ec : n->elifs)
                        if (blockHasYield(ec.body)) return true;
                    if (n->elseBody && blockHasYield(*n->elseBody)) return true;
                    break;
                }
                case StmtKind::While:
                    if (blockHasYield(
                        static_cast<const WhileStmt*>(s.get())->body))
                        return true;
                    break;
                case StmtKind::For:
                    if (blockHasYield(
                        static_cast<const ForStmt*>(s.get())->body))
                        return true;
                    break;
                case StmtKind::Try: {
                    auto* n = static_cast<const TryStmt*>(s.get());
                    if (blockHasYield(n->tryBody)) return true;
                    for (auto& h : n->handlers)
                        if (blockHasYield(h.body)) return true;
                    if (n->finallyBody && blockHasYield(*n->finallyBody))
                        return true;
                    break;
                }
                default: break;
                }
            }
            return false;
            };
        def->isGenerator = blockHasYield(def->body);
        return def;
    }

    StmtPtr Parser::parseImport() {
        Token imp = advance();
        Token name = expect(TokenType::Identifier, "module name");
        std::string alias;
        if (match(TokenType::As)) {
            Token a = expect(TokenType::Identifier, "alias after 'as'");
            alias = a.lexeme;
        }
        return std::make_unique<ImportStmt>(name.lexeme, alias, imp.location);
    }
    // Phase 15.0: extern "C":
    //     def puts(s: str) -> int
    //     def strlen(s: str) -> int
    StmtPtr Parser::parseExtern() {
        Token eTok = advance();
        if (check(TokenType::String)) {
            advance();
        }
        else {
            throw ParseError("expected string ABI name after 'extern'",
                peek().location);
        }
        expect(TokenType::Colon, "':' after extern ABI");
        expect(TokenType::Newline, "newline after ':'");
        expect(TokenType::Indent, "indented extern block");

        auto node = std::make_unique<ExternBlockStmt>("C", eTok.location);

        for (;;) {
            skipNewlines();
            if (isAtEnd() || check(TokenType::Dedent)) break;

            if (!check(TokenType::Def))
                throw ParseError(
                    "expected 'def' inside extern block", peek().location);
            Token defTok = advance();
            Token nameTok = expect(TokenType::Identifier, "function name");

            ExternFnDecl fn;
            fn.name = nameTok.lexeme;
            fn.loc = defTok.location;

            expect(TokenType::LParen, "'(' after function name");
            if (!check(TokenType::RParen)) {
                fn.params.push_back(parseParam());
                while (match(TokenType::Comma)) {
                    if (check(TokenType::RParen)) break;
                    fn.params.push_back(parseParam());
                }
            }
            expect(TokenType::RParen, "')' to close parameter list");

            if (match(TokenType::Arrow))
                fn.returnType = parseTypeExpr();
            else
                fn.returnType = std::make_unique<NameRefExpr>("None",
                    defTok.location);

            node->funcs.push_back(std::move(fn));
            if (!check(TokenType::Newline) && !check(TokenType::Dedent) &&
                !isAtEnd())
                throw ParseError("unexpected token after extern declaration",
                    peek().location);
        }
        match(TokenType::Dedent);
        if (node->funcs.empty())
            throw ParseError("extern block has no declarations", eTok.location);
        return node;
    }

    StmtPtr Parser::parseFromImport() {
        Token fromTok = advance();
        Token module = expect(TokenType::Identifier, "module name");
        expect(TokenType::Import, "'import' after module name");

        std::vector<ImportItem> items;
        for (;;) {
            ImportItem it;
            Token n = expect(TokenType::Identifier, "name to import");
            it.name = n.lexeme;
            if (match(TokenType::As)) {
                Token a = expect(TokenType::Identifier, "alias after 'as'");
                it.alias = a.lexeme;
            }
            items.push_back(std::move(it));
            if (!match(TokenType::Comma)) break;
        }
        return std::make_unique<FromImportStmt>(module.lexeme,
            std::move(items), fromTok.location);
    }

    // Phase 11.1j: `public` / `private` / `protected` soft keywords.
    bool Parser::consumeVisibility(Visibility& out) {
        if (!check(TokenType::Identifier)) return false;
        const std::string& w = peek().lexeme;
        if (w == "public") { advance(); out = Visibility::Public; return true; }
        if (w == "private") { advance(); out = Visibility::Private; return true; }
        if (w == "protected") { advance(); out = Visibility::Protected; return true; }
        return false;
    }

    FieldDef Parser::parseFieldDef() {
        Token name = expect(TokenType::Identifier, "field name");
        expect(TokenType::Colon, "':' after field name");
        ExprPtr type = parseTypeExpr();
        FieldDef f;
        f.name = name.lexeme;
        f.type = std::move(type);
        f.loc = name.location;
        if (!check(TokenType::Newline) && !check(TokenType::Dedent) &&
            !isAtEnd())
            throw ParseError("unexpected token after field type",
                peek().location);
        return f;
    }

    StmtPtr Parser::parseStruct() {
        Token stTok = advance();
        Token name = expect(TokenType::Identifier, "struct name");
        expect(TokenType::Colon, "':' after struct name");
        expect(TokenType::Newline, "newline after ':'");
        expect(TokenType::Indent, "indented struct body");
        std::vector<FieldDef> fields;
        for (;;) {
            skipNewlines();
            if (isAtEnd() || check(TokenType::Dedent)) break;
            fields.push_back(parseFieldDef());
        }
        match(TokenType::Dedent);
        if (fields.empty())
            throw ParseError("struct '" + name.lexeme + "' has no fields",
                stTok.location);
        return std::make_unique<StructStmt>(name.lexeme, std::move(fields),
            stTok.location);
    }

    StmtPtr Parser::parseClass() {
        Token cTok = advance();
        Token name = expect(TokenType::Identifier, "class name");

        // Phase 11.2b: optional type parameter list on classes.
        std::vector<std::string> typeParams;
        std::vector<std::string> typeParamConstraints;
        if (match(TokenType::Lt)) {
            for (;;) {
                Token tp = expect(TokenType::Identifier,
                    "type parameter name");
                std::string constraint;
                if (match(TokenType::Colon)) {
                    Token c = expect(TokenType::Identifier,
                        "constraint name");
                    constraint = c.lexeme;
                }
                typeParams.push_back(tp.lexeme);
                typeParamConstraints.push_back(std::move(constraint));
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::Gt, "'>' to close type parameter list");
        }

        std::string parentName;
        if (match(TokenType::LParen)) {
            Token p = expect(TokenType::Identifier, "parent class name");
            parentName = p.lexeme;
            expect(TokenType::RParen, "')' after parent class");
        }
        expect(TokenType::Colon, "':' after class name");
        expect(TokenType::Newline, "newline after ':'");
        expect(TokenType::Indent, "indented class body");

        auto cls = std::make_unique<ClassStmt>(name.lexeme, parentName,
            cTok.location);
        cls->typeParams = std::move(typeParams);
        cls->typeParamConstraints = std::move(typeParamConstraints);

        for (;;) {
            skipNewlines();
            if (isAtEnd() || check(TokenType::Dedent)) break;

            // Phase 11.1j: optional visibility modifier.
            Visibility vis = Visibility::Public;
            consumeVisibility(vis);

            // Phase 11.1c: static member declaration.
            if (check(TokenType::Identifier) && peek().lexeme == "static" &&
                peek(1).type == TokenType::Identifier) {
                advance();
                Token nm = expect(TokenType::Identifier, "static member name");
                StaticFieldDef sf;
                sf.name = nm.lexeme;
                sf.loc = nm.location;
                sf.vis = vis;
                if (match(TokenType::Colon)) sf.type = parseTypeExpr();
                if (match(TokenType::Assign)) sf.init = parseExpression();
                if (!sf.type && !sf.init)
                    throw ParseError("static member '" + sf.name +
                        "' needs a type or an initializer", nm.location);
                cls->staticFields.push_back(std::move(sf));
                continue;
            }

            if (check(TokenType::Def)) {
                auto m = parseDef();
                if (m->params.empty() || m->params[0].name != "self")
                    throw ParseError("method '" + m->name +
                        "' must have 'self' as its first parameter", m->loc);
                m->vis = vis;
                cls->methods.push_back(std::move(m));
            }
            else if (check(TokenType::Identifier) &&
                peek(1).type == TokenType::Colon) {
                FieldDef f = parseFieldDef();
                f.vis = vis;
                cls->fields.push_back(std::move(f));
            }
            else {
                throw ParseError("expected field or method in class body",
                    peek().location);
            }
        }
        match(TokenType::Dedent);
        if (cls->fields.empty() && cls->staticFields.empty() &&
            cls->methods.empty())
            throw ParseError("class '" + name.lexeme + "' is empty",
                cTok.location);
        for (auto& f : cls->fields)
            for (auto& m : cls->methods)
                if (f.name == m->name)
                    throw ParseError("'" + f.name +
                        "' declared as both field and method", f.loc);
        return cls;
    }

    // =========================================================================
    // Type expressions
    // =========================================================================

    static bool isTypeStart(TokenType t) {
        switch (t) {
        case TokenType::Identifier:
        case TokenType::IntKw:   case TokenType::FloatKw: case TokenType::BoolKw:
        case TokenType::StrKw:   case TokenType::CharKw:  case TokenType::BytesKw:
        case TokenType::Ptr:     case TokenType::Ref:
        case TokenType::Unique:  case TokenType::Shared:  case TokenType::Weak:
            return true;
        default: return false;
        }
    }

    ExprPtr Parser::parseTypeExpr() {
        const Token& t = peek();
        if (!isTypeStart(t.type))
            throw ParseError(std::string("expected type name, found '") +
                t.lexeme + "'", t.location);
        Token name = advance();

        // If the next token isn't '<', this is a bare type name.  Otherwise
        // it's a generic instantiation.  We must not short-circuit on
        // "primitive type name", because `unique`, `shared`, and `weak` are
        // keyword tokens that legitimately take a type argument.
        if (!check(TokenType::Lt))
            return std::make_unique<NameRefExpr>(name.lexeme, name.location);

        advance();   // '<'
        auto node = std::make_unique<GenericTypeExpr>(name.lexeme,
            name.location);
        node->typeArgs.push_back(parseTypeExpr());
        while (match(TokenType::Comma))
            node->typeArgs.push_back(parseTypeExpr());
        expect(TokenType::Gt, "'>' to close type argument list");
        return node;
    }

    // =========================================================================
    // Expressions
    // =========================================================================

    ExprPtr Parser::parseExpression() {
        if (check(TokenType::Lambda)) return parseLambda();
        return parseBinary(1);
    }

    ExprPtr Parser::parseLambda() {
        Token lamTok = advance();
        auto node = std::make_unique<LambdaExpr>(lamTok.location);
        if (!check(TokenType::Colon)) {
            Token p = expect(TokenType::Identifier, "lambda parameter name");
            node->params.push_back(p.lexeme);
            while (match(TokenType::Comma)) {
                p = expect(TokenType::Identifier, "lambda parameter name");
                node->params.push_back(p.lexeme);
            }
        }
        expect(TokenType::Colon, "':' after lambda parameters");
        node->body = parseExpression();
        return node;
    }

    ExprPtr Parser::parseBinary(int minPrec) {
        ExprPtr left = parseUnary();
        for (;;) {
            int prec = binPrec(peek().type);
            if (prec < minPrec) break;
            Token opTok = advance();
            BinOp op = tokenToBinOp(opTok.type);
            int nextMin = (op == BinOp::Pow) ? prec : prec + 1;
            ExprPtr right = parseBinary(nextMin);
            left = std::make_unique<BinaryExpr>(op, std::move(left),
                std::move(right), opTok.location);
        }
        return left;
    }

    ExprPtr Parser::parseUnary() {
        if (match(TokenType::Minus)) {
            Token t = previous();
            return std::make_unique<UnaryExpr>(UnOp::Neg, parseUnary(),
                t.location);
        }
        if (match(TokenType::Plus)) {
            Token t = previous();
            return std::make_unique<UnaryExpr>(UnOp::Pos, parseUnary(),
                t.location);
        }
        if (match(TokenType::Not)) {
            Token t = previous();
            return std::make_unique<UnaryExpr>(UnOp::Not, parseUnary(),
                t.location);
        }
        if (match(TokenType::Tilde)) {
            Token t = previous();
            return std::make_unique<UnaryExpr>(UnOp::BNot, parseUnary(),
                t.location);
        }
        return parsePostfix();
    }

    CallArg Parser::parseCallArg() {
        CallArg a; a.loc = peek().location;
        if (check(TokenType::Identifier) && peek(1).type == TokenType::Assign) {
            a.name = advance().lexeme;
            advance();
            a.value = parseExpression();
            return a;
        }
        a.value = parseExpression();
        return a;
    }

    ExprPtr Parser::parsePostfix() {
        ExprPtr e = parsePrimary();
        for (;;) {
            if (match(TokenType::Dot)) {
                Token name = peek();
                if (name.lexeme.empty())
                    throw ParseError("expected attribute name after '.'",
                        name.location);
                char c0 = name.lexeme[0];
                bool identish = (c0 == '_' ||
                    (c0 >= 'a' && c0 <= 'z') ||
                    (c0 >= 'A' && c0 <= 'Z'));
                if (!identish)
                    throw ParseError(
                        std::string("expected attribute name after '.', "
                            "found '") + name.lexeme + "'",
                        name.location);
                advance();

                // Phase 12.1: `x.upgrade()` on a weak<T> is erased to `x`.
                // weak<T> shares the same runtime representation as T, so
                // the desugar here makes `.upgrade()` a total no-op that
                // every backend understands without any new opcodes.
                if (name.lexeme == "upgrade" &&
                    check(TokenType::LParen) &&
                    peek(1).type == TokenType::RParen) {
                    advance();   // '('
                    advance();   // ')'
                    continue;
                }

                // Phase 11.1h: `Namespace.member` -> NameRef("Ns.member").
                if (e->kind == ExprKind::NameRef) {
                    auto* nre = static_cast<NameRefExpr*>(e.get());
                    if (namespaceNames_.count(nre->name)) {
                        std::string full = nre->name + "." + name.lexeme;
                        e = std::make_unique<NameRefExpr>(full, nre->loc);
                        continue;
                    }
                }

                e = std::make_unique<AttrExpr>(std::move(e), name.lexeme,
                    name.location);
            }
            else if (check(TokenType::LParen)) {
                Token open = advance();
                std::vector<CallArg> args;
                if (!check(TokenType::RParen)) {
                    args.push_back(parseCallArg());
                    while (match(TokenType::Comma)) {
                        if (check(TokenType::RParen)) break;
                        args.push_back(parseCallArg());
                    }
                }
                expect(TokenType::RParen, "')' to close argument list");
                e = std::make_unique<CallExpr>(std::move(e),
                    std::move(args), open.location);
            }
            else if (check(TokenType::LBracket)) {
                Token open = advance();
                ExprPtr startE;
                ExprPtr endE;
                bool isSlice = false;

                if (!check(TokenType::Colon) && !check(TokenType::RBracket)) {
                    startE = parseExpression();
                }
                if (match(TokenType::Colon)) {
                    isSlice = true;
                    if (!check(TokenType::RBracket))
                        endE = parseExpression();
                }
                expect(TokenType::RBracket, "']' to close index/slice");

                if (isSlice) {
                    e = std::make_unique<SliceExpr>(std::move(e),
                        std::move(startE), std::move(endE), open.location);
                }
                else {
                    if (!startE)
                        throw ParseError("empty [] index", open.location);
                    e = std::make_unique<IndexExpr>(std::move(e),
                        std::move(startE), open.location);
                }
            }
            else break;
        }
        return e;
    }

    ExprPtr Parser::parseListLit() {
        Token open = advance();
        auto node = std::make_unique<ListLitExpr>(open.location);
        if (!check(TokenType::RBracket)) {
            node->elements.push_back(parseExpression());
            while (match(TokenType::Comma)) {
                if (check(TokenType::RBracket)) break;
                node->elements.push_back(parseExpression());
            }
        }
        expect(TokenType::RBracket, "']' to close list literal");
        return node;
    }

    ExprPtr Parser::parseBraceLit() {
        Token open = advance();
        if (check(TokenType::RBrace)) {
            advance();
            return std::make_unique<MapLitExpr>(open.location);
        }
        ExprPtr first = parseExpression();
        if (check(TokenType::Colon)) {
            advance();
            auto m = std::make_unique<MapLitExpr>(open.location);
            ExprPtr v = parseExpression();
            m->entries.push_back({ std::move(first), std::move(v) });
            while (match(TokenType::Comma)) {
                if (check(TokenType::RBrace)) break;
                ExprPtr k2 = parseExpression();
                expect(TokenType::Colon, "':' between key and value");
                ExprPtr v2 = parseExpression();
                m->entries.push_back({ std::move(k2), std::move(v2) });
            }
            expect(TokenType::RBrace, "'}' to close map literal");
            return m;
        }
        auto s = std::make_unique<SetLitExpr>(open.location);
        s->elements.push_back(std::move(first));
        while (match(TokenType::Comma)) {
            if (check(TokenType::RBrace)) break;
            s->elements.push_back(parseExpression());
        }
        expect(TokenType::RBrace, "'}' to close set literal");
        return s;
    }

    ExprPtr Parser::parsePrimary() {
        const Token& t = peek();
        switch (t.type) {
        case TokenType::Int: {
            Token tok = advance();
            long long v = parseIntLiteralText(tok.lexeme, tok.location);
            return std::make_unique<IntLitExpr>(v, tok.lexeme, tok.location);
        }
        case TokenType::Float: {
            Token tok = advance();
            double v = 0;
            try { v = std::stod(tok.lexeme); }
            catch (...) {
                throw ParseError("bad float literal", tok.location);
            }
            return std::make_unique<FloatLitExpr>(v, tok.lexeme,
                tok.location);
        }
        case TokenType::String: {
            Token tok = advance();
            return std::make_unique<StringLitExpr>(tok.lexeme, tok.location);
        }
        case TokenType::Char: {
            Token tok = advance();
            return std::make_unique<CharLitExpr>(tok.lexeme, tok.location);
        }
        case TokenType::True: {
            Token tok = advance();
            return std::make_unique<BoolLitExpr>(true, tok.location);
        }
        case TokenType::False: {
            Token tok = advance();
            return std::make_unique<BoolLitExpr>(false, tok.location);
        }
        case TokenType::None: {
            Token tok = advance();
            return std::make_unique<NoneLitExpr>(tok.location);
        }
        case TokenType::Identifier:
        case TokenType::IntKw: case TokenType::FloatKw:
        case TokenType::BoolKw:
        case TokenType::StrKw: case TokenType::CharKw:
        case TokenType::BytesKw:
        case TokenType::Ptr: case TokenType::Ref: case TokenType::Unique:
        case TokenType::Shared: case TokenType::Weak: {
            Token tok = advance();
            return std::make_unique<NameRefExpr>(tok.lexeme, tok.location);
        }
        case TokenType::LParen: {
            Token open = advance();
            if (check(TokenType::RParen)) {
                advance();
                return std::make_unique<TupleLitExpr>(open.location);
            }
            ExprPtr first = parseExpression();
            if (check(TokenType::Comma)) {
                auto tup = std::make_unique<TupleLitExpr>(open.location);
                tup->elements.push_back(std::move(first));
                while (match(TokenType::Comma)) {
                    if (check(TokenType::RParen)) break;
                    tup->elements.push_back(parseExpression());
                }
                expect(TokenType::RParen, "')' to close tuple literal");
                return tup;
            }
            expect(TokenType::RParen, "')' to close group");
            return std::make_unique<GroupingExpr>(std::move(first),
                open.location);
        }
        case TokenType::LBracket: return parseListLit();
        case TokenType::LBrace:   return parseBraceLit();
        default:
            throw ParseError(std::string("expected expression, found '") +
                t.lexeme + "'", t.location);
        }
    }

} // namespace vayu