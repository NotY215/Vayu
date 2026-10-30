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
    };

} // namespace vib