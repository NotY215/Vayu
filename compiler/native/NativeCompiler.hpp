#pragma once
#include "ast/Ast.hpp"
#include <string>

namespace vayu {

    class NativeCompiler {
    public:
        NativeCompiler();

        int  compileAndRun(const Block& program, const std::string& sourceDir = "");
        void dumpIR(const Block& program, const std::string& sourceDir = "");

        /// If set, compileAndRun writes the executable here and does NOT run it.
        void setOutputExe(const std::string& path) { outputExe_ = path; }

        /// Optimisation level passed to gcc: 0..3.  Default 2.
        void setOptLevel(int n) { if (n >= 0 && n <= 3) optLevel_ = n; }

        /// Dump the embedded native runtime to `path`.  Used by the fixpoint
        /// harness (vayuc --emit-runtime).
        bool writeRuntimeC(const std::string& path) const;

        const std::string& lastError() const { return lastError_; }
        void setQbePath(const std::string& p) { qbePath_ = p; }
        void setCcPath(const std::string& p) { ccPath_ = p; }
        void setQbeTarget(const std::string& t) { qbeTarget_ = t; }

    private:
        std::string lastError_;
        std::string qbePath_;
        std::string ccPath_;
        std::string qbeTarget_;
        std::string outputExe_;
        int         optLevel_ = 2;
        std::string buildQBE(const Block& program, const std::string& sourceDir);
    };

} // namespace vayu