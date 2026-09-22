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

        // Read headers line by line.  Lines are \r\n terminated per spec,
        // but accept bare \n too.
        for (;;) {
            std::string line;
            if (!std::getline(in_, line)) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;  // blank line: end of headers
            if (line.rfind("Content-Length:", 0) == 0) {
                const char* p = line.c_str() + 15;
                while (*p == ' ') ++p;
                contentLength = (size_t)std::strtoul(p, nullptr, 10);
                haveLength = true;
            }
            // Ignore Content-Type and other headers.
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
    // Dispatch
    // ------------------------------------------------------------------

    static JsonPtr makeRange(int line0, int col0) {
        auto r = jsonObject();
        auto s = jsonObject();
        jsonSet(s, "line", jsonInt(line0));
        jsonSet(s, "character", jsonInt(col0));
        auto e = jsonObject();
        jsonSet(e, "line", jsonInt(line0));
        jsonSet(e, "character", jsonInt(col0 + 1));
        jsonSet(r, "start", s);
        jsonSet(r, "end", e);
        return r;
    }

    static JsonPtr makeDiagnostic(int line1, int col1, const std::string& msg) {
        auto d = jsonObject();
        jsonSet(d, "range", makeRange(line1 - 1, col1 - 1));
        jsonSet(d, "severity", jsonInt(1));
        jsonSet(d, "source", jsonString("vayu"));
        jsonSet(d, "message", jsonString(msg));
        return d;
    }

    void LspServer::handleMessage(const std::string& body) {
        auto j = parseJson(body);
        if (!j || !j->isObject()) throw std::runtime_error("not a JSON-RPC message");

        auto idNode = j->get("id");
        auto methodNode = j->get("method");
        auto paramsNode = j->get("params");

        if (!methodNode || !methodNode->isString()) {
            // Response from client; ignore.
            return;
        }
        const std::string& method = methodNode->asString();

        int id = -1;
        bool hasId = false;
        if (idNode) {
            hasId = true;
            if (idNode->isInt()) id = (int)idNode->asInt();
        }

        if (method == "initialize") {
            onInitialize(id, paramsNode);
            return;
        }
        if (method == "initialized") return;
        if (method == "shutdown") {
            onShutdown(id);
            return;
        }
        if (method == "exit") {
            // Client sends exit; we return from run() shortly by
            // pretending EOF.  Simplest: std::exit.
            std::exit(shutdownReceived_ ? 0 : 1);
        }
        if (method == "textDocument/didOpen") {
            onDidOpen(paramsNode);
            return;
        }
        if (method == "textDocument/didChange") {
            onDidChange(paramsNode);
            return;
        }
        if (method == "textDocument/didClose") {
            onDidClose(paramsNode);
            return;
        }
        // Unknown request with id -> reply method not found.
        if (hasId) {
            sendError(id, -32601, "method not found: " + method);
        }
    }

    // ------------------------------------------------------------------
    // LSP methods
    // ------------------------------------------------------------------

    void LspServer::onInitialize(int id, const JsonPtr&) {
        auto caps = jsonObject();
        auto sync = jsonObject();
        jsonSet(sync, "openClose", jsonBool(true));
        jsonSet(sync, "change", jsonInt(1));  // full sync
        jsonSet(caps, "textDocumentSync", sync);

        auto info = jsonObject();
        jsonSet(info, "name", jsonString("vls"));
        jsonSet(info, "version", jsonString("0.1.0"));

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
        // Full sync: last change has the new full text.
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
    // Diagnostics
    // ------------------------------------------------------------------

    void LspServer::publishDiagnostics(const std::string& uri,
        const std::string& text) {
        auto diags = jsonArray();

        try {
            vayu::Lexer lexer(text);
            auto tokens = lexer.tokenize();

            // Surface lexer-level Invalid tokens first.
            for (const auto& t : tokens) {
                if (t.type == vayu::TokenType::Invalid) {
                    jsonPush(diags, makeDiagnostic(
                        t.location.line, t.location.column,
                        t.lexeme.empty() ? std::string("invalid token")
                        : t.lexeme));
                }
            }

            vayu::Parser parser(std::move(tokens));
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