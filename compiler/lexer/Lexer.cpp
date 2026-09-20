#include "Lexer.hpp"
#include <cctype>

namespace vayu {

    // NOTE: "self" is intentionally NOT a keyword. The class parser enforces
    // the naming convention; the lexer treats it as a plain identifier.
    //
    // Phase 11: only `const` and `enum` are hard keywords for now.  Words
    // that are commonly used as identifiers or method names (match, case,
    // defer, namespace, static, public, private, protected) will be handled
    // as soft keywords by the parser when each feature lands — the lexer
    // leaves them as Identifier.
    const std::unordered_map<std::string, TokenType>& Lexer::keywords() {
        static const std::unordered_map<std::string, TokenType> kw = {
            {"def", TokenType::Def}, {"return", TokenType::Return},
            {"if", TokenType::If}, {"elif", TokenType::Elif}, {"else", TokenType::Else},
            {"while", TokenType::While}, {"for", TokenType::For},
            {"break", TokenType::Break}, {"continue", TokenType::Continue},
            {"pass", TokenType::Pass},
            {"class", TokenType::Class}, {"struct", TokenType::Struct},
            {"enum", TokenType::Enum}, {"interface", TokenType::Interface},
            {"new", TokenType::New}, {"delete", TokenType::Delete},
            {"import", TokenType::Import}, {"from", TokenType::From}, {"as", TokenType::As},
            {"try", TokenType::Try}, {"except", TokenType::Except},
            {"finally", TokenType::Finally}, {"with", TokenType::With},
            {"raise", TokenType::Raise},
            {"unsafe", TokenType::Unsafe}, {"spawn", TokenType::Spawn},
            {"wait", TokenType::Wait}, {"task", TokenType::Task},
            {"async", TokenType::Async}, {"await", TokenType::Await},
            {"and", TokenType::And}, {"or", TokenType::Or}, {"not", TokenType::Not},
            {"in", TokenType::In}, {"is", TokenType::Is},
            {"lambda", TokenType::Lambda}, {"yield", TokenType::Yield},
            {"true", TokenType::True}, {"false", TokenType::False}, {"none", TokenType::None},
            {"True", TokenType::True}, {"False", TokenType::False}, {"None", TokenType::None},
            {"int", TokenType::IntKw}, {"float", TokenType::FloatKw},
            {"bool", TokenType::BoolKw}, {"str", TokenType::StrKw},
            {"char", TokenType::CharKw}, {"bytes", TokenType::BytesKw},
            {"ptr", TokenType::Ptr}, {"ref", TokenType::Ref},
            {"unique", TokenType::Unique}, {"shared", TokenType::Shared},
            {"weak", TokenType::Weak},

            // Phase 11.1b — hard keyword only.
            {"const", TokenType::Const},
            {"extern", TokenType::Extern},
        };
        return kw;
    }

    Lexer::Lexer(std::string source) : source_(std::move(source)) {
        indentStack_.push_back(0);
    }

    char Lexer::peek(int ahead) const {
        size_t i = pos_ + static_cast<size_t>(ahead);
        return i < source_.size() ? source_[i] : '\0';
    }

    char Lexer::advance() {
        char c = source_[pos_++];
        if (c == '\n') { line_++; col_ = 1; }
        else { col_++; }
        return c;
    }

    bool Lexer::match(char expected) {
        if (isAtEnd() || source_[pos_] != expected) return false;
        advance();
        return true;
    }

    SourceLocation Lexer::here() const {
        return SourceLocation{ line_, col_, pos_ };
    }

    void Lexer::add(TokenType type, std::string lexeme) {
        tokens_.push_back(Token{ type, std::move(lexeme), here() });
    }

    void Lexer::addAt(TokenType type, SourceLocation loc, std::string lexeme) {
        tokens_.push_back(Token{ type, std::move(lexeme), loc });
    }

    std::vector<Token> Lexer::tokenize() {
        while (!isAtEnd()) {
            if (atLineStart_ && bracketDepth_ == 0) {
                handleLineStart();
                if (isAtEnd()) break;
            }

            char c = peek();

            if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }

            if (c == '\n') {
                if (bracketDepth_ == 0
                    && !tokens_.empty()
                    && tokens_.back().type != TokenType::Newline) {
                    add(TokenType::Newline, "\n");
                }
                advance();
                atLineStart_ = true;
                continue;
            }

            if (c == '#') { skipComment(); continue; }

            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { readIdentifier(); continue; }
            if (std::isdigit(static_cast<unsigned char>(c))) { readNumber();     continue; }
            if (c == '"' || c == '\'') { readString(c);    continue; }

            readOperator();
        }

        if (!tokens_.empty() && tokens_.back().type != TokenType::Newline) {
            add(TokenType::Newline, "\n");
        }
        while (indentStack_.size() > 1) {
            indentStack_.pop_back();
            add(TokenType::Dedent);
        }
        add(TokenType::EndOfFile);
        return std::move(tokens_);
    }

    void Lexer::handleLineStart() {
        atLineStart_ = false;

        int indent = 0;
        while (!isAtEnd()) {
            char c = peek();
            if (c == ' ') { indent += 1; advance(); }
            else if (c == '\t') { indent += 4; advance(); }
            else break;
        }

        // Blank line?  Accept \n, \r\n, or \r alone.  THIS was the CRLF bug.
        if (isAtEnd() || peek() == '\n' || peek() == '\r') return;

        if (peek() == '#') return;

        int current = indentStack_.back();
        if (indent > current) {
            indentStack_.push_back(indent);
            add(TokenType::Indent);
        }
        else if (indent < current) {
            while (indentStack_.size() > 1 && indentStack_.back() > indent) {
                indentStack_.pop_back();
                add(TokenType::Dedent);
            }
            if (indentStack_.back() != indent) {
                add(TokenType::Invalid, "inconsistent indentation");
            }
        }
    }

    void Lexer::skipComment() {
        while (!isAtEnd() && peek() != '\n') advance();
    }

    void Lexer::readIdentifier() {
        SourceLocation start = here();
        size_t begin = pos_;
        while (!isAtEnd()) {
            char c = peek();
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') advance();
            else break;
        }
        std::string text = source_.substr(begin, pos_ - begin);
        auto it = keywords().find(text);
        TokenType type = (it != keywords().end()) ? it->second : TokenType::Identifier;
        tokens_.push_back(Token{ type, std::move(text), start });
    }

    void Lexer::readNumber() {
        SourceLocation start = here();
        size_t begin = pos_;

        // Phase 11.1i: 0b/0B, 0o/0O, 0x/0X integer prefixes.
        if (peek() == '0') {
            char n1 = peek(1);
            if (n1 == 'b' || n1 == 'B' ||
                n1 == 'o' || n1 == 'O' ||
                n1 == 'x' || n1 == 'X') {
                advance();  // '0'
                advance();  // prefix letter
                auto isDigitForBase = [&](char c) -> bool {
                    if (n1 == 'b' || n1 == 'B') return c == '0' || c == '1';
                    if (n1 == 'o' || n1 == 'O') return c >= '0' && c <= '7';
                    return (c >= '0' && c <= '9') ||
                        (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F');
                    };
                bool any = false;
                while (!isAtEnd() && isDigitForBase(peek())) { advance(); any = true; }
                if (!any) {
                    addAt(TokenType::Invalid, start,
                        std::string("expected digits after '0") + n1 + "'");
                    return;
                }
                std::string text = source_.substr(begin, pos_ - begin);
                tokens_.push_back(Token{ TokenType::Int, std::move(text), start });
                return;
            }
        }

        bool isFloat = false;

        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();

        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            isFloat = true;
            advance();
            while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }

        if (peek() == 'e' || peek() == 'E') {
            int ahead = 1;
            if (peek(ahead) == '+' || peek(ahead) == '-') ahead++;
            if (std::isdigit(static_cast<unsigned char>(peek(ahead)))) {
                isFloat = true;
                advance();
                if (peek() == '+' || peek() == '-') advance();
                while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
            }
        }

        std::string text = source_.substr(begin, pos_ - begin);
        tokens_.push_back(Token{ isFloat ? TokenType::Float : TokenType::Int,
                                 std::move(text), start });
    }

    // ------------------------------------------------------------------
    // String / char literals.  Escape sequences ARE decoded:
    //   \n \t \r \0 \\ \" \'  -> the corresponding byte
    //   anything else         -> the escaped character itself (i.e. "\x" -> 'x')
    // ------------------------------------------------------------------
    void Lexer::readString(char quote) {
        SourceLocation start = here();
        advance();  // opening quote

        std::string body;
        while (!isAtEnd() && peek() != quote) {
            if (peek() == '\\' && peek(1) != '\0') {
                advance();              // consume backslash
                char e = advance();     // consume escape letter
                switch (e) {
                case 'n':  body += '\n'; break;
                case 't':  body += '\t'; break;
                case 'r':  body += '\r'; break;
                case '0':  body += '\0'; break;
                case '\\': body += '\\'; break;
                case '"':  body += '"';  break;
                case '\'': body += '\''; break;
                default:   body += e;    break;
                }
                continue;
            }
            if (peek() == '\n') break;
            body += advance();
        }

        if (isAtEnd() || peek() != quote) {
            tokens_.push_back(Token{ TokenType::Invalid,
                                     "unterminated string literal", start });
            return;
        }
        advance();  // closing quote

        TokenType type = (quote == '\'') ? TokenType::Char : TokenType::String;
        tokens_.push_back(Token{ type, std::move(body), start });
    }

    void Lexer::readOperator() {
        SourceLocation start = here();
        char c = advance();
        switch (c) {
        case '+':
            if (match('=')) addAt(TokenType::PlusAssign, start, "+=");
            else            addAt(TokenType::Plus, start, "+");
            return;
        case '-':
            if (match('=')) { addAt(TokenType::MinusAssign, start, "-="); return; }
            if (match('>')) { addAt(TokenType::Arrow, start, "->"); return; }
            addAt(TokenType::Minus, start, "-"); return;
        case '*':
            if (match('*')) {
                if (match('=')) { addAt(TokenType::StarStarAssign, start, "**="); return; }
                addAt(TokenType::StarStar, start, "**"); return;
            }
            if (match('=')) { addAt(TokenType::StarAssign, start, "*="); return; }
            addAt(TokenType::Star, start, "*"); return;
        case '/':
            if (match('/')) {
                if (match('=')) { addAt(TokenType::SlashSlashAssign, start, "//="); return; }
                addAt(TokenType::SlashSlash, start, "//"); return;
            }
            if (match('=')) { addAt(TokenType::SlashAssign, start, "/="); return; }
            addAt(TokenType::Slash, start, "/"); return;
        case '%':
            if (match('=')) { addAt(TokenType::PercentAssign, start, "%="); return; }
            addAt(TokenType::Percent, start, "%"); return;
        case '=':
            if (match('=')) { addAt(TokenType::Eq, start, "=="); return; }
            if (match('>')) { addAt(TokenType::FatArrow, start, "=>"); return; }
            addAt(TokenType::Assign, start, "="); return;
        case '!':
            if (match('=')) { addAt(TokenType::NotEq, start, "!="); return; }
            addAt(TokenType::Invalid, start, "unexpected '!'"); return;
        case '<':
            if (match('<')) {
                if (match('=')) { addAt(TokenType::ShlAssign, start, "<<="); return; }
                addAt(TokenType::Shl, start, "<<"); return;
            }
            if (match('=')) { addAt(TokenType::LtEq, start, "<="); return; }
            addAt(TokenType::Lt, start, "<"); return;
        case '>':
            if (match('>')) {
                if (match('=')) { addAt(TokenType::ShrAssign, start, ">>="); return; }
                addAt(TokenType::Shr, start, ">>"); return;
            }
            if (match('=')) { addAt(TokenType::GtEq, start, ">="); return; }
            addAt(TokenType::Gt, start, ">"); return;
        case '.':
            if (match('.')) { addAt(TokenType::Invalid, start, "unexpected '..'"); return; }
            addAt(TokenType::Dot, start, "."); return;
        case ',': addAt(TokenType::Comma, start, ","); return;
        case ':': addAt(TokenType::Colon, start, ":"); return;
        case ';': addAt(TokenType::Semicolon, start, ";"); return;
        case '(': bracketDepth_++; addAt(TokenType::LParen, start, "("); return;
        case ')': if (bracketDepth_ > 0) bracketDepth_--; addAt(TokenType::RParen, start, ")"); return;
        case '[': bracketDepth_++; addAt(TokenType::LBracket, start, "["); return;
        case ']': if (bracketDepth_ > 0) bracketDepth_--; addAt(TokenType::RBracket, start, "]"); return;
        case '{': bracketDepth_++; addAt(TokenType::LBrace, start, "{"); return;
        case '}': if (bracketDepth_ > 0) bracketDepth_--; addAt(TokenType::RBrace, start, "}"); return;
        case '&':
            if (match('=')) { addAt(TokenType::AmpAssign, start, "&="); return; }
            addAt(TokenType::Amp, start, "&"); return;
        case '|':
            if (match('=')) { addAt(TokenType::PipeAssign, start, "|="); return; }
            addAt(TokenType::Pipe, start, "|"); return;
        case '^':
            if (match('=')) { addAt(TokenType::CaretAssign, start, "^="); return; }
            addAt(TokenType::Caret, start, "^"); return;
        case '~': addAt(TokenType::Tilde, start, "~"); return;
        case '@': addAt(TokenType::At, start, "@"); return;
        default: {
            std::string bad(1, c);
            addAt(TokenType::Invalid, start, std::move(bad));
            return;
        }
        }
    }

} // namespace vayu