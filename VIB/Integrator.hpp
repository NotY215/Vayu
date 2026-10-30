#pragma once
#include "compiler/ast/Ast.hpp"
#include <string>
#include <vector>

namespace vib {

    enum class Feature {
        BasicArithmetic,
        Strings,
        Functions,
        ControlFlow,
        Collections,
        Modules,
        Exceptions,
        Classes,
        Generics,
        Generators,
        Pointers,
        VIR,
        Verifier,
        RuntimeEmission,
        NativeCodegen,
        VyePackaging,
        ArtifactCache,
        IncrementalBuild,
        SelfHosting,
    };

    enum class SupportLevel { NotSupported, Scaffolded, Deferred, Supported };
    enum class Backend { Interpreter, VM, Native, Vye };
    enum class ErrorKind {
        None, Unsupported, CompilationError, VerifierError,
        RuntimeError, LoadingError, LinkingError, ArtifactError,
    };

    struct CapabilityInfo {
        Feature     feature;
        const char* name;
        SupportLevel nativeLevel;
        SupportLevel vyeLevel;
        int         plannedPhase;
        const char* note;
    };

    struct CompileResult {
        bool        success = false;
        ErrorKind   kind = ErrorKind::None;
        Feature     feature = Feature::NativeCodegen;
        int         plannedPhase = 0;
        std::string message;
        std::string artifactPath;
    };

    struct DumpIRResult {
        bool        success = false;
        ErrorKind   kind = ErrorKind::None;
        Feature     feature = Feature::VIR;
        int         plannedPhase = 0;
        std::string message;
        std::string irText;
    };

    // Phase 2A.2 — result of emitting a .vye.
    struct EmitVyeResult {
        bool        success = false;
        ErrorKind   kind = ErrorKind::None;
        std::string message;
        std::string artifactPath;
        size_t      bytesWritten = 0;
    };

    // Phase 2B.1/2B.2 — result of loading and/or executing a .vye.
    struct RunVyeResult {
        bool        success = false;
        ErrorKind   kind = ErrorKind::None;
        std::string message;
        int         exitCode = 0;
        std::vector<std::string> stdoutLines;
    };

    // Phase 2C.3 — cache configuration.
    struct CacheConfig {
        bool        enabled = false;
        std::string cacheDir;   // defaults to .vayu-cache relative to CWD
        bool        verbose = false;
    };

    class Integrator {
    public:
        static const std::vector<CapabilityInfo>& capabilities();
        static CapabilityInfo capability(Feature f);
        static const char* featureName(Feature f);
        static const char* supportLevelName(SupportLevel s);
        static const char* backendName(Backend b);
        static const char* errorKindName(ErrorKind k);

        CompileResult compile(const vayu::Block& program,
            const std::string& sourceDir,
            const std::string& outputPath) const;

        DumpIRResult dumpIR(const vayu::Block& program,
            const std::string& sourceDir) const;

        CompileResult emitRuntime(const std::string& outputPath) const;

        // Phase 2A.2 — deterministic .vye writer.
        EmitVyeResult emitVye(const vayu::Block& program,
            const std::string& sourceDir,
            const std::string& vyePath) const;

        // Phase 2B.1/2B.2 — strict .vye reader + execution.
        RunVyeResult runVye(const std::string& vyePath) const;

        // Phase 2C.3 — cache configuration (applied to subsequent compile/emitVye).
        void setCacheConfig(const CacheConfig& cfg) { cache_ = cfg; }
        const CacheConfig& cacheConfig() const { return cache_; }

    private:
        CacheConfig cache_;
    };

} // namespace vib