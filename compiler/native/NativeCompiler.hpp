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

        const std::string& lastError() const { return lastError_; }
        void setQbePath(const std::string& p) { qbePath_ = p; }
        void setCcPath(const std::string& p) { ccPath_ = p; }
        void setQbeTarget(const std::string& t) { qbeTarget_ = t; }

        /// Phase F: dump the embedded full runtime to a path.  Used by the
        /// fixpoint harness so it does not need the stripped vayu_rt.c.
        bool writeRuntimeC(const std::string& path) const;

    private:
        std::string lastError_;
        std::string qbePath_;
        std::string ccPath_;
        std::string qbeTarget_;
        std::string outputExe_;
        std::string buildQBE(const Block& program, const std::string& sourceDir);
    };

} // namespace vayu