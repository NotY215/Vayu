#pragma once
#include "compiler/ast/Ast.hpp"
#include <string>
#include <vector>

namespace vib {

    // Every capability VIB/Integrator either has, is scaffolding, or has
    // deferred. Ordered by planned-phase-then-alphabetical so the runtime
    // status table is stable and greppable.
    enum class Feature {
        // Integrator language subset (lowering planned in Phases 3-10).
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
        // Integrator internals.
        VIR,
        Verifier,
        RuntimeEmission,
        NativeCodegen,
        // Deferred capabilities.
        VyePackaging,
        ArtifactCache,
        IncrementalBuild,
        SelfHosting,
    };

    // How far along a given (Feature, Backend) pair actually is.
    enum class SupportLevel {
        NotSupported,   // Not started for this backend.
        Scaffolded,     // API seam exists; every call fails cleanly.
        Deferred,       // Explicitly deferred per project decision.
        Supported,      // End-to-end implemented and tested.
    };

    // Execution routes the Integrator knows about. Interpreter and VM are
    // currently external to Integrator; they will be adopted as dispatch
    // targets in Phase 2C. Native and Vye are Integrator-owned from the start.
    enum class Backend {
        Interpreter,
        VM,
        Native,
        Vye,
    };

    // Kinds of failure a CompileResult can describe. Needed so callers can
    // distinguish "not implemented yet" from a real compile/verify/runtime
    // failure once Phase 2+ lands.
    enum class ErrorKind {
        None,
        Unsupported,
        CompilationError,
        VerifierError,
        RuntimeError,
        LoadingError,
        LinkingError,
        ArtifactError,
    };

    // One row in the capability matrix.
    struct CapabilityInfo {
        Feature     feature;
        const char* name;
        SupportLevel nativeLevel;
        SupportLevel vyeLevel;
        int         plannedPhase;
        const char* note;
    };

    // Result of any Integrator operation. Carries enough metadata for a future
    // artifact cache (Phase 2B) to key on without an API change.
    struct CompileResult {
        bool        success = false;
        ErrorKind   kind = ErrorKind::None;
        Feature     feature = Feature::NativeCodegen;
        int         plannedPhase = 0;
        std::string message;
        std::string artifactPath;
    };

    class Integrator {
    public:
        // Static capability query API — the single source of truth for every
        // capability question in the toolchain. Nothing outside VIB/ should
        // hardcode a "not implemented" message.
        static const std::vector<CapabilityInfo>& capabilities();
        static CapabilityInfo capability(Feature f);
        static const char* featureName(Feature f);
        static const char* supportLevelName(SupportLevel s);
        static const char* backendName(Backend b);
        static const char* errorKindName(ErrorKind k);

        // Compilation seam. Currently fails with ErrorKind::Unsupported because
        // Feature::NativeCodegen is Scaffolded. Becomes the real entry point
        // when Phase 12 lands, with no API change.
        CompileResult compile(const vayu::Block& program,
            const std::string& sourceDir,
            const std::string& outputPath) const;

        // VIR dump seam. Currently fails with ErrorKind::Unsupported because
        // Feature::VIR is Scaffolded (Phase 2).
        CompileResult dumpIR(const vayu::Block& program,
            const std::string& sourceDir) const;

        // Runtime emission seam. Currently fails with ErrorKind::Unsupported
        // because Feature::RuntimeEmission is Scaffolded (Phase 12).
        CompileResult emitRuntime(const std::string& outputPath) const;
    };

} // namespace vib