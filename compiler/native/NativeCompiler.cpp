#include "NativeCompiler.hpp"
#include "VIB/Integrator.hpp"
namespace vayu {

    NativeCompiler::NativeCompiler() = default;

    int NativeCompiler::compileAndRun(const Block& program,
        const std::string& sourceDir) {
        lastError_.clear();
        vib::Integrator integrator;
        integrator.setCacheConfig(cache_);
        const vib::CompileResult r = integrator.compile(program, sourceDir, outputExe_);
        lastError_ = r.message;
        return r.success ? 0 : 1;
    }

    std::string NativeCompiler::dumpIR(const Block& program,
        const std::string& sourceDir) {
        lastError_.clear();
        vib::Integrator integrator;
        const vib::DumpIRResult r = integrator.dumpIR(program, sourceDir);
        if (!r.success) {
            lastError_ = r.message;
            return {};
        }
        return r.irText;
    }

    int NativeCompiler::emitVye(const Block& program,
        const std::string& sourceDir,
        const std::string& vyePath) {
        lastError_.clear();
        vib::Integrator integrator;
        integrator.setCacheConfig(cache_);
        const vib::EmitVyeResult r = integrator.emitVye(program, sourceDir, vyePath);
        lastError_ = r.message;
        return r.success ? 0 : 1;
    }

    int NativeCompiler::runVye(const std::string& vyePath) {
        lastError_.clear();
        stdout_.clear();
        vib::Integrator integrator;
        const vib::RunVyeResult r = integrator.runVye(vyePath);
        lastError_ = r.message;
        stdout_ = r.stdoutLines;
        return r.success ? 0 : 1;
    }

    bool NativeCompiler::writeRuntimeC(const std::string& path) {
        lastError_.clear();
        vib::Integrator integrator;
        const vib::CompileResult r = integrator.emitRuntime(path);
        if (!r.success) {
            lastError_ = r.message;
            return false;
        }
        return true;
    }

} // namespace vayu