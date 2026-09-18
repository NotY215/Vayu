#pragma once
#include <string>
#include <cstddef>

namespace vayu {

    // Phase 11.1j: member visibility.
    enum class Visibility { Public, Protected, Private };

    enum class TokenType {
        // --- Literals ---
        Int, Float, String, Char,
        True, False, None,

        // --- Identifier ---
        Identifier,

        // --- Keywords ---
        Def, Return, If, Elif, Else, While, For, Break, Continue, Pass,
        Class, Struct, Enum, Interface, Self, New, Delete,
        Import, From, As,
        Try, Except, Finally, With, Raise,
        Unsafe, Spawn, Wait, Task, Async, Await,
        And, Or, Not, In, Is, Lambda, Yield,
        IntKw, FloatKw, BoolKw, StrKw, CharKw, BytesKw,
        Ptr, Ref, Unique, Shared, Weak,
        Const,
        Extern,

        // --- Operators / punctuation ---
        Plus, Minus, Star, Slash, Percent, StarStar, SlashSlash,
        Assign, Eq, NotEq, Lt, Gt, LtEq, GtEq,
        PlusAssign, MinusAssign, StarAssign, SlashAssign,
        // Phase 11.1i — additional compound assigns
        PercentAssign, StarStarAssign, SlashSlashAssign,
        AmpAssign, PipeAssign, CaretAssign, ShlAssign, ShrAssign,
        Arrow, FatArrow,
        Dot, Comma, Colon, Semicolon,
        LParen, RParen, LBracket, RBracket, LBrace, RBrace,
        Amp, Pipe, Caret, Tilde, Shl, Shr, At,

        // --- Structural ---
        Newline, Indent, Dedent, EndOfFile, Invalid,
    };

    struct SourceLocation {
        int    line = 1;
        int    column = 1;
        size_t offset = 0;
    };

    struct Token {
        TokenType     type;
        std::string   lexeme;
        SourceLocation location;
    };

    const char* tokenTypeName(TokenType type);

} // namespace vayu