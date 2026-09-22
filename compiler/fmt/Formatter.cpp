// compiler/fmt/Formatter.cpp
#include "Formatter.hpp"
#include "lexer/Lexer.hpp"
#include "lexer/Token.hpp"
#include "parser/Parser.hpp"
#include "ast/Ast.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace vayu::fmt {

    // Pass 1: tabs -> spaces; strip trailing whitespace; collapse 3+ blanks to 2.
    static std::string passWhitespace(const std::string& source, int indentWidth) {
        std::string step1;
        step1.reserve(source.size());
        size_t i = 0;
        const size_t n = source.size();
        while (i < n) {
            size_t eol = i;
            while (eol < n && source[eol] != '\n') ++eol;

            std::string line = source.substr(i, eol - i);

            std::string expanded;
            expanded.reserve(line.size() + 8);
            size_t j = 0;
            while (j < line.size()) {
                char c = line[j];
                if (c == '\t') {
                    expanded.append((size_t)indentWidth, ' ');
                }
                else if (c == ' ') {
                    expanded.push_back(' ');
                }
                else {
                    break;
                }
                ++j;
            }
            expanded.append(line, j, line.size() - j);

            while (!expanded.empty() &&
                (expanded.back() == ' ' || expanded.back() == '\t' ||
                    expanded.back() == '\r')) {
                expanded.pop_back();
            }
            step1 += expanded;
            step1 += '\n';
            i = (eol < n) ? eol + 1 : n;
        }

        std::string step2;
        step2.reserve(step1.size());
        int blanks = 0;
        i = 0;
        while (i < step1.size()) {
            size_t eol = i;
            while (eol < step1.size() && step1[eol] != '\n') ++eol;
            bool isBlank = (eol == i);
            if (isBlank) {
                ++blanks;
                if (blanks <= 2) step2 += '\n';
            }
            else {
                blanks = 0;
                step2.append(step1, i, eol - i);
                step2 += '\n';
            }
            i = (eol < step1.size()) ? eol + 1 : step1.size();
        }

        while (!step2.empty() && step2.back() == '\n') step2.pop_back();
        step2 += '\n';
        return step2;
    }

    // Pass 2: reindent each line to the level the tokenizer assigned.
    // The lexer's Indent/Dedent tokens are authoritative for block structure.
    static std::string passReindent(const std::string& src, int indentWidth) {
        vayu::Lexer lexer(src);
        auto tokens = lexer.tokenize();

        // Map line number (1-based) -> indent level.
        std::vector<int> lineLevel(1, 0);
        int maxLine = 0;
        for (const auto& t : tokens) {
            if ((int)t.location.line > maxLine) maxLine = (int)t.location.line;
        }
        lineLevel.assign((size_t)maxLine + 2, -1);

        int level = 0;
        for (const auto& t : tokens) {
            if (t.type == vayu::TokenType::Indent)  ++level;
            else if (t.type == vayu::TokenType::Dedent) { if (level > 0) --level; }
            else if (t.type == vayu::TokenType::Newline ||
                t.type == vayu::TokenType::EndOfFile) {
                // no-op
            }
            else {
                int ln = (int)t.location.line;
                if (ln < (int)lineLevel.size() && lineLevel[(size_t)ln] < 0) {
                    lineLevel[(size_t)ln] = level;
                }
            }
        }

        // Rebuild: for each source line, strip leading ws, prepend
        // indentWidth * level spaces.  Blank and comment-only lines keep
        // their original leading whitespace.
        std::string out;
        out.reserve(src.size());
        size_t i = 0;
        int lineNo = 1;
        const size_t n = src.size();
        while (i < n) {
            size_t eol = i;
            while (eol < n && src[eol] != '\n') ++eol;
            std::string line = src.substr(i, eol - i);

            bool isBlank = true;
            for (char c : line) {
                if (c != ' ' && c != '\t' && c != '\r') { isBlank = false; break; }
            }
            bool isComment = false;
            {
                size_t k = 0;
                while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
                if (k < line.size() && line[k] == '#') isComment = true;
            }

            if (isBlank || isComment) {
                out += line;
                out += '\n';
            }
            else {
                size_t k = 0;
                while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
                std::string body = line.substr(k);

                int lvl = 0;
                if (lineNo < (int)lineLevel.size() && lineLevel[(size_t)lineNo] > 0)
                    lvl = lineLevel[(size_t)lineNo];

                out.append((size_t)(indentWidth * lvl), ' ');
                out += body;
                out += '\n';
            }

            ++lineNo;
            i = (eol < n) ? eol + 1 : n;
        }
        return out;
    }

    // Pass 3: token-aware spacing — comma, `=`, `==`, and multi-space collapse.
    static std::string passSpacing(const std::string& src) {
        std::string out;
        out.reserve(src.size() + 32);

        bool inString = false;
        char stringDelim = 0;
        bool inComment = false;
        bool escape = false;
        bool leadingWs = true;

        const size_t n = src.size();
        for (size_t i = 0; i < n; ++i) {
            char c = src[i];

            if (inComment) {
                out += c;
                if (c == '\n') { inComment = false; leadingWs = true; }
                continue;
            }
            if (inString) {
                out += c;
                if (escape) { escape = false; continue; }
                if (c == '\\') { escape = true;  continue; }
                if (c == stringDelim) inString = false;
                continue;
            }
            if (c == '#') { inComment = true; out += c; continue; }
            if (c == '"' || c == '\'') {
                inString = true; stringDelim = c; out += c; leadingWs = false;
                continue;
            }

            if (c == '\n') { out += c; leadingWs = true; continue; }

            if (leadingWs && (c == ' ' || c == '\t')) { out += c; continue; }

            leadingWs = false;

            // Collapse 2+ spaces to 1 (or drop entirely before a closer).
            if (c == ' ') {
                size_t j = i;
                while (j < n && src[j] == ' ') ++j;
                char next = (j < n) ? src[j] : '\0';
                if (next == '\0' || next == '\n' || next == ')' || next == ']' ||
                    next == '}' || next == ',' || next == ':' || next == '#') {
                    i = j - 1;
                    continue;
                }
                // "foo (" -> "foo(" ; drop space before '('
                if (next == '(') { i = j - 1; continue; }
                out += ' ';
                i = j - 1;
                continue;
            }

            // "foo() :" -> "foo():" (drop space before block colon)
            if (c == ':' && !out.empty() && out.back() == ' ') {
                while (!out.empty() && out.back() == ' ') out.pop_back();
                out += ':';
                continue;
            }

            // Two-char comparison operators.
            if (i + 1 < n) {
                char d = src[i + 1];
                bool isCmp2 =
                    (c == '=' && d == '=') ||
                    (c == '!' && d == '=') ||
                    (c == '<' && d == '=') ||
                    (c == '>' && d == '=');
                if (isCmp2) {
                    while (!out.empty() && out.back() == ' ') out.pop_back();
                    out += ' '; out += c; out += d; out += ' ';
                    size_t j = i + 2;
                    while (j < n && src[j] == ' ') ++j;
                    i = j - 1;
                    continue;
                }
            }

            // Plain '=' assignment.
            if (c == '=') {
                bool prevOp = (i > 0 && (src[i - 1] == '=' || src[i - 1] == '<' ||
                    src[i - 1] == '>' || src[i - 1] == '!' ||
                    src[i - 1] == '+' || src[i - 1] == '-' ||
                    src[i - 1] == '*' || src[i - 1] == '/' ||
                    src[i - 1] == '%' || src[i - 1] == '&' ||
                    src[i - 1] == '|' || src[i - 1] == '^'));
                bool nextEq = (i + 1 < n && src[i + 1] == '=');
                if (!prevOp && !nextEq) {
                    while (!out.empty() && out.back() == ' ') out.pop_back();
                    out += " = ";
                    size_t j = i + 1;
                    while (j < n && src[j] == ' ') ++j;
                    i = j - 1;
                    continue;
                }
            }

            // Comma -> ", ".
            if (c == ',') {
                out += ',';
                size_t j = i + 1;
                while (j < n && (src[j] == ' ' || src[j] == '\t')) ++j;
                if (j < n && src[j] != ')' && src[j] != ']' && src[j] != '}' &&
                    src[j] != '\n' && src[j] != '#') {
                    out += ' ';
                    i = j - 1;
                }
                continue;
            }

            out += c;
        }
        return out;
    }

    Result process(const std::string& source, const Options& opts) {
        Result r;

        try {
            vayu::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            vayu::Parser parser(std::move(tokens));
            vayu::Block program = parser.parseProgram();
            (void)program;
        }
        catch (const vayu::ParseError& e) {
            r.ok = false; r.exitCode = 1;
            std::ostringstream ss;
            ss << "parse error at line " << e.loc.line << ":"
                << e.loc.column << ": " << e.what();
            r.error = ss.str();
            return r;
        }

        if (opts.checkOnly) { r.ok = true; return r; }

        std::string step1 = passWhitespace(source, opts.indentWidth);
        std::string step2 = passReindent(step1, opts.indentWidth);
        std::string step3 = passSpacing(step2);

        r.ok = true;
        r.formatted = std::move(step3);
        return r;
    }

    void printUsage() {
        std::cerr <<
            "usage: vfmt <file.vyu> [--check] [--in-place]\n"
            "  --check      parse only; exit non-zero on syntax errors\n"
            "  --in-place   rewrite file with normalized whitespace\n"
            "  (default)    print formatted source to stdout\n";
    }

    int run(int argc, char** argv) {
        if (argc < 2) { printUsage(); return 2; }

        Options opts;
        std::string path;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--check") == 0) {
                opts.checkOnly = true;
            }
            else if (std::strcmp(argv[i], "--in-place") == 0) {
                opts.inPlace = true;
            }
            else if (argv[i][0] == '-') {
                std::cerr << "vfmt: unknown flag " << argv[i] << "\n";
                return 2;
            }
            else {
                if (!path.empty()) {
                    std::cerr << "vfmt: only one file at a time\n";
                    return 2;
                }
                path = argv[i];
            }
        }
        if (path.empty()) { printUsage(); return 2; }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "vfmt: cannot read " << path << "\n";
            return 2;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();

        auto r = process(src, opts);
        if (!r.ok) {
            std::cerr << path << ": " << r.error << "\n";
            return r.exitCode;
        }
        if (opts.checkOnly) return 0;

        if (opts.inPlace) {
            if (r.formatted == src) return 0;
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                std::cerr << "vfmt: cannot write " << path << "\n";
                return 2;
            }
            out << r.formatted;
            return 0;
        }

        std::cout << r.formatted;
        return 0;
    }

}