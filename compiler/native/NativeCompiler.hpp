#pragma once
#include "ast/Ast.hpp"
#include <string>

namespace vayu {

    // Native compiler.  VCB is the only backend.
    //
    // The QBE + gcc pipeline that older versions used has been
    // removed.  The flow is:
    //
    //   AST -> VcbLower -> .vcbir text -> vcb.exe -> .exe
    //
    // `setVcbPath` overrides the default `tools\vcb.exe`.  The env
    // var `VAYU_VCB` also overrides it, provided the value contains a
    // path separator (a bare filename is ignored on purpose — cmd.exe
    // would search PATH and produce unhelpful errors).
    class NativeCompiler {
    public:
        NativeCompiler();

        int  compileAndRun(const Block& program, const std::string& sourceDir = "");
        void dumpIR(const Block& program, const std::string& sourceDir = "");

        void setOutputExe(const std::string& path) { outputExe_ = path; }
        void setVcbPath(const std::string& p) { vcbPath_ = p; }

        // Kept for CLI compatibility: writes a stub explaining that
        // the native runtime is embedded in VCB.
        bool writeRuntimeC(const std::string& path) const;

        const std::string& lastError() const { return lastError_; }

    private:
        std::string lastError_;
        std::string vcbPath_;
        std::string outputExe_;

        std::string resolveVcbPath() const;
    };

} // namespace vayu