#pragma once
#include "ast/Ast.hpp"
#include <string>

namespace vayu {

    // Native compiler.  VCB is the only backend.
    //
    // As of Phase 28.4 the default flow is:
    //
    //   AST -> VcbLower -> .vcbir
    //        -> vcb emit-obj -> .obj
    //        -> vcb link     -> .exe
    //
    // With --link=embedded the pipeline falls back to `vcb build`,
    // which invokes VCB's own PE/ELF writer with no external linker.
    class NativeCompiler {
    public:
        enum class LinkMode {
            External,  // default: emit-obj + external link via lld-link
            Embedded,  // --link=embedded: vcb build (VCB's own PE writer)
        };

        NativeCompiler();

        int  compileAndRun(const Block& program, const std::string& sourceDir = "");
        void dumpIR(const Block& program, const std::string& sourceDir = "");

        void setOutputExe(const std::string& path) { outputExe_ = path; }
        void setVcbPath(const std::string& p) { vcbPath_ = p; }
        void setLinkMode(LinkMode m) { linkMode_ = m; }

        // Kept for CLI compatibility: writes a stub explaining that
        // the native runtime is embedded in VCB.
        bool writeRuntimeC(const std::string& path) const;

        const std::string& lastError() const { return lastError_; }

    private:
        std::string lastError_;
        std::string vcbPath_;
        std::string outputExe_;
        LinkMode    linkMode_ = LinkMode::External;

        std::string resolveVcbPath() const;
    };

} // namespace vayu