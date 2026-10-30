#include "Integrator.hpp"
#include "Vir.hpp"
#include "VirInterp.hpp"
#include <cstdio>

namespace vib {

    namespace {

        const std::vector<CapabilityInfo>& table() {
            static const std::vector<CapabilityInfo> t = {
                { Feature::BasicArithmetic,  "BasicArithmetic",  SupportLevel::Supported,    SupportLevel::Deferred,  3, "Phase 3: integer/bool literals, arithmetic, comparisons, simple assignment." },
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
                { Feature::VIR,              "VIR",              SupportLevel::Supported,    SupportLevel::Deferred,  2, "Phase 2: structures, verifier, printer, minimal lowering (PassStmt)." },
                { Feature::Verifier,         "Verifier",         SupportLevel::Supported,    SupportLevel::Deferred,  2, "Phase 2: structural checks; expands as VIR subset grows." },
                { Feature::RuntimeEmission,  "RuntimeEmission",  SupportLevel::Scaffolded,   SupportLevel::Deferred, 12, "API stub present; runtime emitter pending." },
                { Feature::NativeCodegen,    "NativeCodegen",    SupportLevel::Scaffolded,   SupportLevel::Deferred, 12, "API stub present; native codegen pending." },
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
            r.message = std::string(info.name) + " is not implemented (planned Phase " +
                std::to_string(info.plannedPhase) + "). " + extra;
            return r;
        }

        Feature featureFromName(const std::string& name) {
            for (const auto& c : table()) {
                if (name == c.name) return c.feature;
            }
            return Feature::VIR;
        }

    } // namespace

    const std::vector<CapabilityInfo>& Integrator::capabilities() { return table(); }

    CapabilityInfo Integrator::capability(Feature f) {
        for (const auto& c : table()) if (c.feature == f) return c;
        return CapabilityInfo{ f, "Unknown", SupportLevel::NotSupported,
                               SupportLevel::NotSupported, 0, "" };
    }

    const char* Integrator::featureName(Feature f) { return capability(f).name; }
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

    CompileResult Integrator::compile(const vayu::Block& program,
        const std::string& sourceDir,
        const std::string& outputPath) const {
        (void)sourceDir;
        (void)outputPath;

        // Phase 3.7: Integrator-mediated execution. Lower to VIR, verify, and
        // execute in-memory. This is NOT native code generation — it proves
        // the VIR pipeline works end-to-end. Feature::NativeCodegen stays
        // Scaffolded until Phase 12.
        VirLowering lowering;
        LoweringResult lr = lowering.lower(program);
        if (!lr.success) {
            CompileResult r;
            r.success = false;
            r.kind = ErrorKind::CompilationError;
            r.feature = featureFromName(lr.failedFeatureName);
            r.plannedPhase = lr.plannedPhase;
            r.message = lr.message;
            return r;
        }
        VirVerifier verifier;
        VerifierResult vr = verifier.verify(lr.module);
        if (!vr.success) {
            CompileResult r;
            r.success = false;
            r.kind = ErrorKind::VerifierError;
            r.feature = Feature::Verifier;
            r.plannedPhase = 2;
            r.message = vr.message;
            return r;
        }

        VirInterpreter interp;
        VirExecResult er = interp.execute(lr.module);
        for (const auto& line : er.stdoutLines) {
            std::fputs(line.c_str(), stdout);
            std::fputc('\n', stdout);
        }
        if (!er.success) {
            CompileResult r;
            r.success = false;
            r.kind = er.kind;
            r.feature = Feature::NativeCodegen;
            r.plannedPhase = 12;
            r.message = er.message;
            return r;
        }
        CompileResult r;
        r.success = true;
        r.kind = ErrorKind::None;
        return r;
    }

    DumpIRResult Integrator::dumpIR(const vayu::Block& program,
        const std::string&) const {
        VirLowering lowering;
        LoweringResult lr = lowering.lower(program);
        if (!lr.success) {
            DumpIRResult r;
            r.success = false;
            r.kind = ErrorKind::CompilationError;
            r.feature = featureFromName(lr.failedFeatureName);
            r.plannedPhase = lr.plannedPhase;
            r.message = lr.message;
            return r;
        }
        VirVerifier verifier;
        VerifierResult vr = verifier.verify(lr.module);
        if (!vr.success) {
            DumpIRResult r;
            r.success = false;
            r.kind = ErrorKind::VerifierError;
            r.feature = Feature::Verifier;
            r.plannedPhase = 2;
            r.message = vr.message;
            return r;
        }
        VirPrinter printer;
        DumpIRResult r;
        r.success = true;
        r.kind = ErrorKind::None;
        r.feature = Feature::VIR;
        r.irText = printer.print(lr.module);
        return r;
    }

    CompileResult Integrator::emitRuntime(const std::string&) const {
        return unsupported(Feature::RuntimeEmission,
            "Runtime source emission is pending. See Roadmap.md.");
    }

} // namespace vib