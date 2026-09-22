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

        void onInitialize(int id, const JsonPtr& params);
        void onShutdown(int id);
        void onDidOpen(const JsonPtr& params);
        void onDidChange(const JsonPtr& params);
        void onDidClose(const JsonPtr& params);
        void onHover(int id, const JsonPtr& params);
        void onDefinition(int id, const JsonPtr& params);
        void onCompletion(int id, const JsonPtr& params);
        void onReferences(int id, const JsonPtr& params);
        void onRename(int id, const JsonPtr& params);
        void onSignatureHelp(int id, const JsonPtr& params);

        void sendResponse(int id, const JsonPtr& result);
        void sendError(int id, int code, const std::string& msg);
        void sendNotification(const std::string& method, const JsonPtr& params);

        void publishDiagnostics(const std::string& uri, const std::string& text);

        bool parseDoc(const std::string& uri,
            std::vector<vayu::Token>& tokens,
            std::shared_ptr<vayu::Block>& program);
    };

} // namespace vayu::lsp