#include "Integrator.hpp"

namespace vib {

    namespace {

        // The single authoritative capability table. Every level and phase below
        // reflects the current project decision. Flipping a cell from Scaffolded
        // or Deferred to Supported requires implementation and tests; the table
        // is not a wish list.
        const std::vector<CapabilityInfo>& table() {
            static const std::vector<CapabilityInfo> t = {
                // Integrator language subset.
                { Feature::BasicArithmetic,  "BasicArithmetic",  SupportLevel::NotSupported, SupportLevel::Deferred,  3, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Strings,          "Strings",          SupportLevel::NotSupported, SupportLevel::Deferred,  4, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Functions,        "Functions",        SupportLevel::NotSupported, SupportLevel::Deferred,  5, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::ControlFlow,      "ControlFlow",      SupportLevel::NotSupported, SupportLevel::Deferred,  6, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Collections,      "Collections",      SupportLevel::NotSupported, SupportLevel::Deferred,  7, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Modules,          "Modules",          SupportLevel::NotSupported, SupportLevel::Deferred,  8, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Exceptions,       "Exceptions",       SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Classes,          "Classes",          SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Generics,         "Generics",         SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Generators,       "Generators",       SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Pointers,         "Pointers",         SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                // Integrator internals.
                { Feature::VIR,              "VIR",              SupportLevel::Scaffolded,   SupportLevel::Deferred,  2, "API stub present; lowering and verifier pending." },
                { Feature::Verifier,         "Verifier",         SupportLevel::Scaffolded,   SupportLevel::Deferred,  2, "Reserved slot; verifier lands with VIR." },
                { Feature::RuntimeEmission,  "RuntimeEmission",  SupportLevel::Scaffolded,   SupportLevel::Deferred, 12, "API stub present; runtime emitter pending." },
                { Feature::NativeCodegen,    "NativeCodegen",    SupportLevel::Scaffolded,   SupportLevel::Deferred, 12, "API stub present; native codegen pending." },
                // Deferred capabilities.
                { Feature::VyePackaging,     "VyePackaging",     SupportLevel::Deferred,     SupportLevel::Deferred,  2, "Deferred per project decision until after Phase 10." },
                { Feature::ArtifactCache,    "ArtifactCache",    SupportLevel::Deferred,     SupportLevel::Deferred, 11, "Deferred until artifact identity is defined." },
                { Feature::IncrementalBuild, "IncrementalBuild", SupportLevel::Deferred,     SupportLevel::Deferred, 11, "Deferred until clean/incremental equivalence is proven." },
                { Feature::SelfHosting,      "SelfHosting",      SupportLevel::Deferred,     SupportLevel::Deferred, 14, "Deferred until Integrator subset is stable." },
            };
            return t;
        }

        CompileResult unsupported(Feature f, const char* extra) {
            const CapabilityInfo info = Integrator::capability(f);
            CompileResult r;
            r.success = false;
            r.kind = ErrorKind::Unsupported;
            r.feature = f;
            r.plannedPhase = info.plannedPhase;
            r.message = std::string(info.name)
                + " is not implemented (planned Phase "
                + std::to_string(info.plannedPhase) + "). "
                + extra;
            return r;
        }

    } // namespace

    const std::vector<CapabilityInfo>& Integrator::capabilities() {
        return table();
    }

    CapabilityInfo Integrator::capability(Feature f) {
        for (const auto& c : table()) {
            if (c.feature == f) return c;
        }
        // Unreachable for any declared Feature; keeps the API total.
        return CapabilityInfo{ f, "Unknown", SupportLevel::NotSupported,
                               SupportLevel::NotSupported, 0, "" };
    }

    const char* Integrator::featureName(Feature f) {
        return capability(f).name;
    }

    const char* Integrator::supportLevelName(SupportLevel s) {
        switch (s) {
        case SupportLevel::NotSupported: return "NotSupported";
        case SupportLevel::Scaffolded:   return "Scaffolded";
        case SupportLevel::Deferred:     return "Deferred";
        case SupportLevel::Supported:    return "Supported";
        }
        return "?";
    }

    const char* Integrator::backendName(Backend b) {
        switch (b) {
        case Backend::Interpreter: return "Interpreter";
        case Backend::VM:          return "VM";
        case Backend::Native:      return "Native";
        case Backend::Vye:         return "Vye";
        }
        return "?";
    }

    const char* Integrator::errorKindName(ErrorKind k) {
        switch (k) {
        case ErrorKind::None:             return "None";
        case ErrorKind::Unsupported:      return "Unsupported";
        case ErrorKind::CompilationError: return "CompilationError";
        case ErrorKind::VerifierError:    return "VerifierError";
        case ErrorKind::RuntimeError:     return "RuntimeError";
        case ErrorKind::LoadingError:     return "LoadingError";
        case ErrorKind::LinkingError:     return "LinkingError";
        case ErrorKind::ArtifactError:    return "ArtifactError";
        }
        return "?";
    }

    CompileResult Integrator::compile(const vayu::Block&,
        const std::string&,
        const std::string&) const {
        return unsupported(Feature::NativeCodegen,
            "Use --run or --vm for execution. See Roadmap.md.");
    }

    CompileResult Integrator::dumpIR(const vayu::Block&,
        const std::string&) const {
        return unsupported(Feature::VIR,
            "VIR lowering and verifier are pending. See Roadmap.md.");
    }

    CompileResult Integrator::emitRuntime(const std::string&) const {
        return unsupported(Feature::RuntimeEmission,
            "Runtime source emission is pending. See Roadmap.md.");
    }

} // namespace vib