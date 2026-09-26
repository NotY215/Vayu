#pragma once
#include "ast/Ast.hpp"
#include <string>

namespace vayu {

    enum class NativeBackend { Qbe, Vcb };

    class NativeCompiler {
    public:
        NativeCompiler();

        int  compileAndRun(const Block& program, const std::string& sourceDir = "");
        void dumpIR(const Block& program, const std::string& sourceDir = "");

        void setOutputExe(const std::string& path) { outputExe_ = path; }
        void setOptLevel(int n) { if (n >= 0 && n <= 3) optLevel_ = n; }
        void setBackend(NativeBackend b) { backend_ = b; }
        NativeBackend backend() const { return backend_; }

        bool writeRuntimeC(const std::string& path) const;

        const std::string& lastError() const { return lastError_; }
        void setQbePath(const std::string& p) { qbePath_ = p; }
        void setCcPath(const std::string& p) { ccPath_ = p; }
        void setQbeTarget(const std::string& t) { qbeTarget_ = t; }
        void setVcbPath(const std::string& p) { vcbPath_ = p; }

    private:
        std::string   lastError_;
        std::string   qbePath_;
        std::string   ccPath_;
        std::string   qbeTarget_;
        std::string   vcbPath_;
        std::string   outputExe_;
        int           optLevel_ = 2;
        NativeBackend backend_ = NativeBackend::Qbe;

        std::string buildQBE(const Block& program, const std::string& sourceDir);

        // VCB path
        int  compileAndRunVcb(const Block& program, const std::string& sourceDir);
        void dumpIRVcb(const Block& program, const std::string& sourceDir);
    };

} // namespace vayu