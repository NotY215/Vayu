#pragma once
#include "ast/Ast.hpp"
#include "VIB/Integrator.hpp"
#include <string>
#include <vector>

namespace vayu {

    class NativeCompiler {
    public:
        enum class LinkMode { External, Embedded };
        NativeCompiler();
        int         compileAndRun(const Block& program, const std::string& sourceDir = "");
        std::string dumpIR(const Block& program, const std::string& sourceDir = "");
        int         emitVye(const Block& program, const std::string& sourceDir,
            const std::string& vyePath);
        int         runVye(const std::string& vyePath);
        void        setOutputExe(const std::string& path) { outputExe_ = path; }
        void        setLinkMode(LinkMode mode) { linkMode_ = mode; }
        void        setCache(bool enabled, const std::string& dir, bool verbose) {
            cache_.enabled = enabled;
            cache_.cacheDir = dir;
            cache_.verbose = verbose;
        }
        bool        writeRuntimeC(const std::string& path);
        const std::string& lastError() const { return lastError_; }
        const std::vector<std::string>& lastStdout() const { return stdout_; }
    private:
        std::string lastError_;
        std::string outputExe_;
        LinkMode    linkMode_ = LinkMode::External;
        vib::CacheConfig cache_;
        std::vector<std::string> stdout_;
    };

} // namespace vayu