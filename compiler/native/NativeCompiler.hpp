#pragma once
#include "ast/Ast.hpp"
#include <string>
namespace vayu {
    class NativeCompiler {
    public:
        enum class LinkMode { External, Embedded };
        NativeCompiler();
        int  compileAndRun(const Block& program, const std::string& sourceDir = "");
        void dumpIR(const Block& program, const std::string& sourceDir = "");
        void setOutputExe(const std::string& path) { outputExe_ = path; }
        void setLinkMode(LinkMode mode) { linkMode_ = mode; }
        bool writeRuntimeC(const std::string& path);          // was: const
        const std::string& lastError() const { return lastError_; }
    private:
        std::string lastError_;
        std::string outputExe_;
        LinkMode    linkMode_ = LinkMode::External;
    };
} // namespace vayu