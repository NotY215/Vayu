#include "NativeCompiler.hpp"
#include "VIB/Integrator.hpp"
#include <cstdio>
namespace vayu {
NativeCompiler::NativeCompiler() = default;
int NativeCompiler::compileAndRun(const Block& program, const std::string& sourceDir) {
    lastError_.clear();
    vib::Integrator integrator;
    const auto result = integrator.compile(program, sourceDir, outputExe_);
    lastError_ = result.message;
    if (!result.success) { std::fprintf(stderr, "VIB: %s\n", lastError_.c_str()); return 1; }
    return 0;
}
void NativeCompiler::dumpIR(const Block&, const std::string&) {
    std::fprintf(stderr, "VIB: VIR dumping is not available yet. See Roadmap.md, Phase 3.\n");
}
bool NativeCompiler::writeRuntimeC(const std::string&) const { return false; }
} // namespace vayu
