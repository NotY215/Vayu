// compiler/lsp/LspServer.hpp
#pragma once
#include "Json.hpp"
#include "ast/Ast.hpp"
#include "lexer/Token.hpp"
#include <istream>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace vayu::lsp {

    class LspServer {
    public:
        LspServer(std::istream& in, std::ostream& out);
        int run();

    private:
        std::istream& in_;
        std::ostream& out_;
        std::unordered_map<std::string, std::string> docs_;
        bool shutdownReceived_ = false;

        bool readMessage(std::string& body);
        void writeMessage(const std::string& body);

        void handleMessage(const std::string& body);

        // Request handlers
        void onInitialize(int id, const JsonPtr& params);
        void onShutdown(int id);
        void onDidOpen(const JsonPtr& params);
        void onDidChange(const JsonPtr& params);
        void onDidClose(const JsonPtr& params);
        void onHover(int id, const JsonPtr& params);
        void onDefinition(int id, const JsonPtr& params);
        void onCompletion(int id, const JsonPtr& params);

        // Response helpers
        void sendResponse(int id, const JsonPtr& result);
        void sendError(int id, int code, const std::string& msg);
        void sendNotification(const std::string& method, const JsonPtr& params);

        // Diagnostics
        void publishDiagnostics(const std::string& uri, const std::string& text);

        // Parse cache for hover/def/completion
        bool parseDoc(const std::string& uri,
            std::vector<vayu::Token>& tokens,
            std::shared_ptr<vayu::Block>& program);
    };

} // namespace vayu::lsp