#include "Integrator.hpp"
namespace vib {
CompileResult Integrator::compile(const vayu::Block&, const std::string&, const std::string&) const {
    return {false, "native compilation is being rebuilt in VIB/Integrator; the VIB native code generator is not implemented yet. Use --run or --vm for execution. See Roadmap.md.", {}};
}
} // namespace vib
