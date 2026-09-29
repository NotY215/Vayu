#pragma once
#include "ast/Ast.hpp"
#include <string>

namespace vayu {

    // Auto: pick VCB when tools\vcb.exe is resolvable from cwd, else
    // fall back to QBE.  Qbe and Vcb force the specific backend.
    enum class NativeBackend { Auto, Qbe, Vcb };

    class NativeCompiler {
    public:
        NativeCompiler();

        int  compileAndRun(const Block& program, const std::string& sourceDir = "");
        void dumpIR(const Block& program, const std::string& sourceDir = "");

        void setOutputExe(const std::string& path) { outputExe_ = path; }
        void setOptLevel(int n) { if (n >= 0 && n <= 3) optLevel_ = n; }
        void setBackend(NativeBackend b) { backend_ = b; }
        NativeBackend backend() const { return backend_; }
        NativeBackend effectiveBackend() const;

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
        NativeBackend backend_ = NativeBackend::Auto;

        std::string buildQBE(const Block& program, const std::string& sourceDir);

        // Returns the resolved relative or absolute path to vcb.exe,
        // or empty if none is found.
        std::string resolveVcbPath() const;

        int  compileAndRunVcb(const Block& program, const std::string& sourceDir);
        void dumpIRVcb(const Block& program, const std::string& sourceDir);
    };

} // namespace vayu