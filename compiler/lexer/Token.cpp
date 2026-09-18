#include "Token.hpp"

namespace vayu {

    const char* tokenTypeName(TokenType t) {
        switch (t) {
        case TokenType::Int:        return "Int";
        case TokenType::Float:      return "Float";
        case TokenType::String:     return "String";
        case TokenType::Char:       return "Char";
        case TokenType::True:       return "True";
        case TokenType::False:      return "False";
        case TokenType::None:       return "None";
        case TokenType::Identifier: return "Identifier";

        case TokenType::Def:       return "Def";
        case TokenType::Return:    return "Return";
        case TokenType::If:        return "If";
        case TokenType::Elif:      return "Elif";
        case TokenType::Else:      return "Else";
        case TokenType::While:     return "While";
        case TokenType::For:       return "For";
        case TokenType::Break:     return "Break";
        case TokenType::Continue:  return "Continue";
        case TokenType::Pass:      return "Pass";
        case TokenType::Class:     return "Class";
        case TokenType::Struct:    return "Struct";
        case TokenType::Enum:      return "Enum";
        case TokenType::Interface: return "Interface";
        case TokenType::Self:      return "Self";
        case TokenType::New:       return "New";
        case TokenType::Delete:    return "Delete";
        case TokenType::Import:    return "Import";
        case TokenType::From:      return "From";
        case TokenType::As:        return "As";
        case TokenType::Try:       return "Try";
        case TokenType::Except:    return "Except";
        case TokenType::Finally:   return "Finally";
        case TokenType::With:      return "With";
        case TokenType::Raise:     return "Raise";
        case TokenType::Unsafe:    return "Unsafe";
        case TokenType::Spawn:     return "Spawn";
        case TokenType::Wait:      return "Wait";
        case TokenType::Task:      return "Task";
        case TokenType::Async:     return "Async";
        case TokenType::Await:     return "Await";
        case TokenType::And:       return "And";
        case TokenType::Or:        return "Or";
        case TokenType::Not:       return "Not";
        case TokenType::In:        return "In";
        case TokenType::Is:        return "Is";
        case TokenType::Lambda:    return "Lambda";
        case TokenType::Yield:     return "Yield";
        case TokenType::IntKw:     return "IntKw";
        case TokenType::FloatKw:   return "FloatKw";
        case TokenType::BoolKw:    return "BoolKw";
        case TokenType::StrKw:     return "StrKw";
        case TokenType::CharKw:    return "CharKw";
        case TokenType::BytesKw:   return "BytesKw";
        case TokenType::Ptr:       return "Ptr";
        case TokenType::Ref:       return "Ref";
        case TokenType::Unique:    return "Unique";
        case TokenType::Shared:    return "Shared";
        case TokenType::Weak:      return "Weak";
        case TokenType::Const:     return "Const";
        case TokenType::Extern:    return "Extern";

        case TokenType::Plus:        return "Plus";
        case TokenType::Minus:       return "Minus";
        case TokenType::Star:        return "Star";
        case TokenType::Slash:       return "Slash";
        case TokenType::Percent:     return "Percent";
        case TokenType::StarStar:    return "StarStar";
        case TokenType::SlashSlash:  return "SlashSlash";
        case TokenType::Assign:      return "Assign";
        case TokenType::Eq:          return "Eq";
        case TokenType::NotEq:       return "NotEq";
        case TokenType::Lt:          return "Lt";
        case TokenType::Gt:          return "Gt";
        case TokenType::LtEq:        return "LtEq";
        case TokenType::GtEq:        return "GtEq";
        case TokenType::PlusAssign:  return "PlusAssign";
        case TokenType::MinusAssign: return "MinusAssign";
        case TokenType::StarAssign:  return "StarAssign";
        case TokenType::SlashAssign: return "SlashAssign";
        case TokenType::PercentAssign:     return "PercentAssign";
        case TokenType::StarStarAssign:    return "StarStarAssign";
        case TokenType::SlashSlashAssign:  return "SlashSlashAssign";
        case TokenType::AmpAssign:         return "AmpAssign";
        case TokenType::PipeAssign:        return "PipeAssign";
        case TokenType::CaretAssign:       return "CaretAssign";
        case TokenType::ShlAssign:         return "ShlAssign";
        case TokenType::ShrAssign:         return "ShrAssign";
        case TokenType::Arrow:       return "Arrow";
        case TokenType::FatArrow:    return "FatArrow";
        case TokenType::Dot:         return "Dot";
        case TokenType::Comma:       return "Comma";
        case TokenType::Colon:       return "Colon";
        case TokenType::Semicolon:   return "Semicolon";
        case TokenType::LParen:      return "LParen";
        case TokenType::RParen:      return "RParen";
        case TokenType::LBracket:    return "LBracket";
        case TokenType::RBracket:    return "RBracket";
        case TokenType::LBrace:      return "LBrace";
        case TokenType::RBrace:      return "RBrace";
        case TokenType::Amp:         return "Amp";
        case TokenType::Pipe:        return "Pipe";
        case TokenType::Caret:       return "Caret";
        case TokenType::Tilde:       return "Tilde";
        case TokenType::Shl:         return "Shl";
        case TokenType::Shr:         return "Shr";
        case TokenType::At:          return "At";

        case TokenType::Newline:   return "Newline";
        case TokenType::Indent:    return "Indent";
        case TokenType::Dedent:    return "Dedent";
        case TokenType::EndOfFile: return "EndOfFile";
        case TokenType::Invalid:   return "Invalid";
        }
        return "?";
    }

} // namespace vayu