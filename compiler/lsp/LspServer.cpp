// compiler/lsp/LspServer.cpp
#include "LspServer.hpp"
#include "Json.hpp"
#include "lexer/Lexer.hpp"
#include "lexer/Token.hpp"
#include "parser/Parser.hpp"
#include "ast/Ast.hpp"
#include "sema/TypeChecker.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace vayu::lsp {

    LspServer::LspServer(std::istream& in, std::ostream& out)
        : in_(in), out_(out) {
    }

    // ------------------------------------------------------------------
    // Content-Length framing
    // ------------------------------------------------------------------

    bool LspServer::readMessage(std::string& body) {
        size_t contentLength = 0;
        bool   haveLength = false;

        for (;;) {
            std::string line;
            if (!std::getline(in_, line)) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.rfind("Content-Length:", 0) == 0) {
                const char* p = line.c_str() + 15;
                while (*p == ' ') ++p;
                contentLength = (size_t)std::strtoul(p, nullptr, 10);
                haveLength = true;
            }
        }
        if (!haveLength) return false;

        body.resize(contentLength);
        if (contentLength > 0) {
            in_.read(&body[0], (std::streamsize)contentLength);
            if ((size_t)in_.gcount() != contentLength) return false;
        }
        return true;
    }

    void LspServer::writeMessage(const std::string& body) {
        out_ << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        out_.flush();
    }

    // ------------------------------------------------------------------
    // Main loop
    // ------------------------------------------------------------------

    int LspServer::run() {
        std::string body;
        while (readMessage(body)) {
            try {
                handleMessage(body);
            }
            catch (const std::exception& e) {
                std::fprintf(stderr, "vls: message error: %s\n", e.what());
            }
        }
        return shutdownReceived_ ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Range helpers
    // ------------------------------------------------------------------

    static JsonPtr makeRange(int line1, int col1, int endCol1) {
        auto r = jsonObject();
        auto s = jsonObject();
        jsonSet(s, "line", jsonInt(line1 - 1));
        jsonSet(s, "character", jsonInt(col1 - 1));
        auto e = jsonObject();
        jsonSet(e, "line", jsonInt(line1 - 1));
        jsonSet(e, "character", jsonInt(endCol1 - 1));
        jsonSet(r, "start", s);
        jsonSet(r, "end", e);
        return r;
    }

    static JsonPtr makeDiagnostic(int line1, int col1, const std::string& msg) {
        auto d = jsonObject();
        jsonSet(d, "range", makeRange(line1, col1, col1 + 1));
        jsonSet(d, "severity", jsonInt(1));
        jsonSet(d, "source", jsonString("vayu"));
        jsonSet(d, "message", jsonString(msg));
        return d;
    }

    // ------------------------------------------------------------------
    // Dispatch
    // ------------------------------------------------------------------

    void LspServer::handleMessage(const std::string& body) {
        auto j = parseJson(body);
        if (!j || !j->isObject()) throw std::runtime_error("not a JSON-RPC message");

        auto idNode = j->get("id");
        auto methodNode = j->get("method");
        auto paramsNode = j->get("params");

        if (!methodNode || !methodNode->isString()) return;

        const std::string& method = methodNode->asString();

        int id = -1;
        bool hasId = false;
        if (idNode) {
            hasId = true;
            if (idNode->isInt()) id = (int)idNode->asInt();
        }

        if (method == "initialize") { onInitialize(id, paramsNode); return; }
        if (method == "initialized")           return;
        if (method == "shutdown") { onShutdown(id); return; }
        if (method == "exit") { std::exit(shutdownReceived_ ? 0 : 1); }
        if (method == "textDocument/didOpen") { onDidOpen(paramsNode); return; }
        if (method == "textDocument/didChange") { onDidChange(paramsNode); return; }
        if (method == "textDocument/didClose") { onDidClose(paramsNode); return; }
        if (method == "textDocument/hover") { onHover(id, paramsNode); return; }
        if (method == "textDocument/definition") { onDefinition(id, paramsNode); return; }
        if (method == "textDocument/completion") { onCompletion(id, paramsNode); return; }

        if (hasId) sendError(id, -32601, "method not found: " + method);
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    void LspServer::onInitialize(int id, const JsonPtr&) {
        auto caps = jsonObject();

        auto sync = jsonObject();
        jsonSet(sync, "openClose", jsonBool(true));
        jsonSet(sync, "change", jsonInt(1));
        jsonSet(caps, "textDocumentSync", sync);

        jsonSet(caps, "hoverProvider", jsonBool(true));
        jsonSet(caps, "definitionProvider", jsonBool(true));

        auto comp = jsonObject();
        auto trigger = jsonArray();
        jsonPush(trigger, jsonString("."));
        jsonSet(comp, "triggerCharacters", trigger);
        jsonSet(caps, "completionProvider", comp);

        auto info = jsonObject();
        jsonSet(info, "name", jsonString("vls"));
        jsonSet(info, "version", jsonString("0.2.0"));

        auto result = jsonObject();
        jsonSet(result, "capabilities", caps);
        jsonSet(result, "serverInfo", info);

        sendResponse(id, result);
    }

    void LspServer::onShutdown(int id) {
        shutdownReceived_ = true;
        sendResponse(id, jsonNull());
    }

    void LspServer::onDidOpen(const JsonPtr& params) {
        if (!params || !params->isObject()) return;
        auto doc = params->get("textDocument");
        if (!doc || !doc->isObject()) return;
        auto uriN = doc->get("uri");
        auto txtN = doc->get("text");
        if (!uriN || !uriN->isString()) return;
        std::string uri = uriN->asString();
        std::string text = (txtN && txtN->isString()) ? txtN->asString() : "";
        docs_[uri] = text;
        publishDiagnostics(uri, text);
    }

    void LspServer::onDidChange(const JsonPtr& params) {
        if (!params || !params->isObject()) return;
        auto doc = params->get("textDocument");
        if (!doc || !doc->isObject()) return;
        auto uriN = doc->get("uri");
        if (!uriN || !uriN->isString()) return;
        std::string uri = uriN->asString();

        auto changes = params->get("contentChanges");
        if (!changes || !changes->isArray() || changes->arr.empty()) return;
        auto last = changes->arr.back();
        if (!last || !last->isObject()) return;
        auto txtN = last->get("text");
        std::string text = (txtN && txtN->isString()) ? txtN->asString() : "";
        docs_[uri] = text;
        publishDiagnostics(uri, text);
    }

    void LspServer::onDidClose(const JsonPtr& params) {
        if (!params || !params->isObject()) return;
        auto doc = params->get("textDocument");
        if (!doc || !doc->isObject()) return;
        auto uriN = doc->get("uri");
        if (!uriN || !uriN->isString()) return;
        std::string uri = uriN->asString();
        docs_.erase(uri);

        auto p = jsonObject();
        jsonSet(p, "uri", jsonString(uri));
        jsonSet(p, "diagnostics", jsonArray());
        sendNotification("textDocument/publishDiagnostics", p);
    }

    // ------------------------------------------------------------------
    // Parse cache
    // ------------------------------------------------------------------

    bool LspServer::parseDoc(const std::string& uri,
        std::vector<vayu::Token>& tokens,
        std::shared_ptr<vayu::Block>& program) {
        auto it = docs_.find(uri);
        if (it == docs_.end()) return false;
        try {
            vayu::Lexer lexer(it->second);
            tokens = lexer.tokenize();
            vayu::Parser parser(tokens);
            program = std::make_shared<vayu::Block>(parser.parseProgram());
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // ------------------------------------------------------------------
    // Position lookup
    // ------------------------------------------------------------------

    static const vayu::Token* findTokenAt(const std::vector<vayu::Token>& tokens,
        int line0, int col0) {
        int tline = line0 + 1;
        int tcol = col0 + 1;
        for (const auto& t : tokens) {
            if (t.location.line != tline) continue;
            int start = t.location.column;
            int len = (int)t.lexeme.size();
            if (len == 0) continue;
            if (tcol >= start && tcol < start + len) return &t;
            if (tcol == start && len == 0) return &t;
        }
        return nullptr;
    }

    static bool isBuiltin(const std::string& n) {
        static const char* names[] = {
            "print","str","bool","int","float","len","abs","type","min","max",
            "input","read_line","read_int","read_all","range","ord","chr",
            "list","map","filter","sorted","reduce","any","all","sum",
            "hash","id","callable","isinstance","issubclass","getattr","hasattr",
            "dir","repr","enumerate","zip","reversed","round","pow","divmod",
            "sign","gcd","lcm","clamp","comb","perm","isqrt","factorial",
            "tuple","set","setattr","delattr","next","malloc","free",
            "read_file","write_file","file_exists","args","run_command","exit",
            "print_raw","join",
            nullptr
        };
        for (int i = 0; names[i]; ++i) if (n == names[i]) return true;
        return false;
    }

    static void collectTopNames(const vayu::Block& b,
        std::vector<std::pair<std::string, vayu::SourceLocation>>& out) {
        for (const auto& s : b.stmts) {
            switch (s->kind) {
            case vayu::StmtKind::Def: {
                auto* d = static_cast<const vayu::DefStmt*>(s.get());
                out.emplace_back(d->name, d->loc);
                break;
            }
            case vayu::StmtKind::Class: {
                auto* d = static_cast<const vayu::ClassStmt*>(s.get());
                out.emplace_back(d->name, d->loc);
                break;
            }
            case vayu::StmtKind::Struct: {
                auto* d = static_cast<const vayu::StructStmt*>(s.get());
                out.emplace_back(d->name, d->loc);
                break;
            }
            case vayu::StmtKind::Const: {
                auto* d = static_cast<const vayu::ConstStmt*>(s.get());
                out.emplace_back(d->name, d->loc);
                break;
            }
            case vayu::StmtKind::Enum: {
                auto* d = static_cast<const vayu::EnumStmt*>(s.get());
                out.emplace_back(d->name, d->loc);
                break;
            }
            default: break;
            }
        }
    }

    // ------------------------------------------------------------------
    // Hover
    // ------------------------------------------------------------------

    void LspServer::onHover(int id, const JsonPtr& params) {
        if (!params || !params->isObject()) { sendResponse(id, jsonNull()); return; }
        auto doc = params->get("textDocument");
        auto pos = params->get("position");
        if (!doc || !pos) { sendResponse(id, jsonNull()); return; }
        auto uriN = doc->get("uri");
        auto lineN = pos->get("line");
        auto colN = pos->get("character");
        if (!uriN || !lineN || !colN) { sendResponse(id, jsonNull()); return; }

        std::string uri = uriN->asString();
        int line0 = (int)lineN->asInt();
        int col0 = (int)colN->asInt();

        std::vector<vayu::Token> tokens;
        std::shared_ptr<vayu::Block> program;
        if (!parseDoc(uri, tokens, program)) { sendResponse(id, jsonNull()); return; }

        const vayu::Token* t = findTokenAt(tokens, line0, col0);
        if (!t) { sendResponse(id, jsonNull()); return; }

        std::string markdown;
        if (t->type == vayu::TokenType::Identifier) {
            std::vector<std::pair<std::string, vayu::SourceLocation>> topNames;
            if (program) collectTopNames(*program, topNames);
            bool found = false;
            for (auto& kv : topNames) {
                if (kv.first == t->lexeme) {
                    markdown = "**Vayu** `" + t->lexeme + "`\n\n*declared at line " +
                        std::to_string(kv.second.line) + "*";
                    found = true;
                    break;
                }
            }
            if (!found && isBuiltin(t->lexeme)) {
                markdown = "**Vayu builtin** `" + t->lexeme + "`";
                found = true;
            }
            if (!found) {
                markdown = "**Vayu identifier** `" + t->lexeme + "`";
            }
        }
        else {
            markdown = "**Vayu** `" + t->lexeme + "`  _(" +
                std::string(vayu::tokenTypeName(t->type)) + ")_";
        }

        auto contents = jsonObject();
        jsonSet(contents, "kind", jsonString("markdown"));
        jsonSet(contents, "value", jsonString(markdown));

        auto result = jsonObject();
        jsonSet(result, "contents", contents);
        jsonSet(result, "range", makeRange(t->location.line, t->location.column,
            t->location.column + (int)t->lexeme.size()));
        sendResponse(id, result);
    }

    // ------------------------------------------------------------------
    // Goto definition
    // ------------------------------------------------------------------

    void LspServer::onDefinition(int id, const JsonPtr& params) {
        if (!params || !params->isObject()) { sendResponse(id, jsonNull()); return; }
        auto doc = params->get("textDocument");
        auto pos = params->get("position");
        if (!doc || !pos) { sendResponse(id, jsonNull()); return; }
        auto uriN = doc->get("uri");
        auto lineN = pos->get("line");
        auto colN = pos->get("character");
        if (!uriN || !lineN || !colN) { sendResponse(id, jsonNull()); return; }

        std::string uri = uriN->asString();
        int line0 = (int)lineN->asInt();
        int col0 = (int)colN->asInt();

        std::vector<vayu::Token> tokens;
        std::shared_ptr<vayu::Block> program;
        if (!parseDoc(uri, tokens, program)) { sendResponse(id, jsonNull()); return; }

        const vayu::Token* t = findTokenAt(tokens, line0, col0);
        if (!t || t->type != vayu::TokenType::Identifier) {
            sendResponse(id, jsonNull()); return;
        }

        std::vector<std::pair<std::string, vayu::SourceLocation>> topNames;
        if (program) collectTopNames(*program, topNames);

        auto arr = jsonArray();
        for (auto& kv : topNames) {
            if (kv.first != t->lexeme) continue;
            auto loc = jsonObject();
            jsonSet(loc, "uri", jsonString(uri));
            jsonSet(loc, "range", makeRange(kv.second.line, kv.second.column,
                kv.second.column + (int)kv.first.size()));
            jsonPush(arr, loc);
            break;
        }

        if (arr->arr.empty()) sendResponse(id, jsonNull());
        else                  sendResponse(id, arr);
    }

    // ------------------------------------------------------------------
    // Completion
    // ------------------------------------------------------------------

    void LspServer::onCompletion(int id, const JsonPtr& params) {
        if (!params || !params->isObject()) {
            auto empty = jsonObject();
            jsonSet(empty, "isIncomplete", jsonBool(false));
            jsonSet(empty, "items", jsonArray());
            sendResponse(id, empty);
            return;
        }
        auto doc = params->get("textDocument");
        auto pos = params->get("position");
        if (!doc || !pos) {
            auto empty = jsonObject();
            jsonSet(empty, "isIncomplete", jsonBool(false));
            jsonSet(empty, "items", jsonArray());
            sendResponse(id, empty);
            return;
        }
        auto uriN = doc->get("uri");
        auto lineN = pos->get("line");
        auto colN = pos->get("character");
        if (!uriN || !lineN || !colN) {
            auto empty = jsonObject();
            jsonSet(empty, "isIncomplete", jsonBool(false));
            jsonSet(empty, "items", jsonArray());
            sendResponse(id, empty);
            return;
        }

        std::string uri = uriN->asString();
        int line0 = (int)lineN->asInt();
        int col0 = (int)colN->asInt();

        auto it = docs_.find(uri);
        if (it == docs_.end()) {
            auto empty = jsonObject();
            jsonSet(empty, "isIncomplete", jsonBool(false));
            jsonSet(empty, "items", jsonArray());
            sendResponse(id, empty);
            return;
        }

        // Extract the identifier prefix ending at the cursor.
        const std::string& text = it->second;
        std::string prefix;
        {
            // Walk backwards from (line0, col0) on the raw text.
            size_t p = 0;
            int curLine = 0;
            while (p < text.size() && curLine < line0) {
                if (text[p] == '\n') ++curLine;
                ++p;
            }
            size_t lineStart = p;
            size_t cursor = lineStart + (size_t)col0;
            if (cursor > text.size()) cursor = text.size();
            size_t scan = cursor;
            while (scan > lineStart) {
                char c = text[scan - 1];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_') {
                    --scan;
                }
                else break;
            }
            prefix = text.substr(scan, cursor - scan);
        }

        auto items = jsonArray();

        // 1. Builtins.
        static const char* builtins[] = {
            "print","str","bool","int","float","len","abs","type","min","max",
            "input","read_line","read_int","read_all","range","ord","chr",
            "list","map","filter","sorted","reduce","any","all","sum",
            "hash","id","callable","isinstance","issubclass","getattr","hasattr",
            "dir","repr","enumerate","zip","reversed","round","pow","divmod",
            "sign","gcd","lcm","clamp","comb","perm","isqrt","factorial",
            "tuple","set","setattr","delattr","next","malloc","free",
            nullptr
        };
        for (int i = 0; builtins[i]; ++i) {
            std::string b = builtins[i];
            if (!prefix.empty() && b.rfind(prefix, 0) != 0) continue;
            auto item = jsonObject();
            jsonSet(item, "label", jsonString(b));
            jsonSet(item, "kind", jsonInt(3));  // Function
            jsonSet(item, "detail", jsonString("builtin"));
            jsonPush(items, item);
        }

        // 2. Top-level names.
        std::vector<vayu::Token> tokens;
        std::shared_ptr<vayu::Block> program;
        if (parseDoc(uri, tokens, program)) {
            std::vector<std::pair<std::string, vayu::SourceLocation>> topNames;
            collectTopNames(*program, topNames);
            for (auto& kv : topNames) {
                if (!prefix.empty() && kv.first.rfind(prefix, 0) != 0) continue;
                auto item = jsonObject();
                jsonSet(item, "label", jsonString(kv.first));
                jsonSet(item, "kind", jsonInt(6));  // Variable
                jsonSet(item, "detail", jsonString("declared at line " +
                    std::to_string(kv.second.line)));
                jsonPush(items, item);
            }
        }

        auto result = jsonObject();
        jsonSet(result, "isIncomplete", jsonBool(false));
        jsonSet(result, "items", items);
        sendResponse(id, result);
    }

    // ------------------------------------------------------------------
    // Diagnostics
    // ------------------------------------------------------------------

    void LspServer::publishDiagnostics(const std::string& uri,
        const std::string& text) {
        auto diags = jsonArray();

        try {
            vayu::Lexer lexer(text);
            auto tokens = lexer.tokenize();

            for (const auto& t : tokens) {
                if (t.type == vayu::TokenType::Invalid) {
                    jsonPush(diags, makeDiagnostic(
                        t.location.line, t.location.column,
                        t.lexeme.empty() ? std::string("invalid token")
                        : t.lexeme));
                }
            }

            vayu::Parser parser(tokens);
            vayu::Block program = parser.parseProgram();

            try {
                vayu::TypeChecker checker;
                checker.check(program);
            }
            catch (const vayu::TypeError& e) {
                jsonPush(diags, makeDiagnostic(
                    e.loc.line, e.loc.column, e.what()));
            }
        }
        catch (const vayu::ParseError& e) {
            jsonPush(diags, makeDiagnostic(
                e.loc.line, e.loc.column, e.what()));
        }
        catch (const std::exception& e) {
            jsonPush(diags, makeDiagnostic(1, 1, e.what()));
        }

        auto params = jsonObject();
        jsonSet(params, "uri", jsonString(uri));
        jsonSet(params, "diagnostics", diags);
        sendNotification("textDocument/publishDiagnostics", params);
    }

    // ------------------------------------------------------------------
    // Send helpers
    // ------------------------------------------------------------------

    void LspServer::sendResponse(int id, const JsonPtr& result) {
        auto msg = jsonObject();
        jsonSet(msg, "jsonrpc", jsonString("2.0"));
        jsonSet(msg, "id", jsonInt(id));
        jsonSet(msg, "result", result);
        writeMessage(serializeJson(msg));
    }

    void LspServer::sendError(int id, int code, const std::string& msg) {
        auto err = jsonObject();
        jsonSet(err, "code", jsonInt(code));
        jsonSet(err, "message", jsonString(msg));
        auto m = jsonObject();
        jsonSet(m, "jsonrpc", jsonString("2.0"));
        jsonSet(m, "id", jsonInt(id));
        jsonSet(m, "error", err);
        writeMessage(serializeJson(m));
    }

    void LspServer::sendNotification(const std::string& method, const JsonPtr& params) {
        auto m = jsonObject();
        jsonSet(m, "jsonrpc", jsonString("2.0"));
        jsonSet(m, "method", jsonString(method));
        jsonSet(m, "params", params);
        writeMessage(serializeJson(m));
    }

} // namespace vayu::lsp