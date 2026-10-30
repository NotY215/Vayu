#include "Integrator.hpp"
#include "Vir.hpp"
#include "VirInterp.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace vib {

    namespace {

        const std::vector<CapabilityInfo>& table() {
            static const std::vector<CapabilityInfo> t = {
                { Feature::BasicArithmetic,  "BasicArithmetic",  SupportLevel::Supported,    SupportLevel::Deferred,  3, "Phase 3: integer/bool literals, arithmetic, comparisons, simple assignment." },
                { Feature::Strings,          "Strings",          SupportLevel::Supported,    SupportLevel::Deferred,  4, "Phase 4: literals, concat, comparisons, methods, len()." },
                { Feature::Functions,        "Functions",        SupportLevel::Supported,    SupportLevel::Deferred,  5, "Phase 5: user functions, calls, recursion, ret plumbing, print, conversions." },
                { Feature::ControlFlow,      "ControlFlow",      SupportLevel::Supported,    SupportLevel::Deferred,  6, "Phase 6.1: if/elif/else, while, for-range, break, continue; ** // and or not str*." },
                { Feature::Collections,      "Collections",      SupportLevel::Supported,    SupportLevel::Deferred,  7, "Phase 7.1–7.2: list/map/tuple/set literals, index get/set, len()." },
                { Feature::Modules,          "Modules",          SupportLevel::Supported,    SupportLevel::Deferred,  8, "Phase 8.1: import/from-import flat function merge from .vyu/.vayu." },
                { Feature::Exceptions,       "Exceptions",       SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Classes,          "Classes",          SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Generics,         "Generics",         SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Generators,       "Generators",       SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::Pointers,         "Pointers",         SupportLevel::NotSupported, SupportLevel::Deferred, 10, "Interpreter and VM support this today; Integrator lowering pending." },
                { Feature::VIR,              "VIR",              SupportLevel::Supported,    SupportLevel::Deferred,  2, "Phase 2: structures, verifier, printer, minimal lowering (PassStmt)." },
                { Feature::Verifier,         "Verifier",         SupportLevel::Supported,    SupportLevel::Deferred,  2, "Phase 2: structural checks; expands as VIR subset grows." },
                { Feature::RuntimeEmission,  "RuntimeEmission",  SupportLevel::Scaffolded,   SupportLevel::Deferred, 12, "API stub present; runtime emitter pending." },
                { Feature::NativeCodegen,    "NativeCodegen",    SupportLevel::Scaffolded,   SupportLevel::Deferred, 12, "API stub present; native codegen pending." },
                { Feature::VyePackaging,     "VyePackaging",     SupportLevel::Supported,    SupportLevel::Supported, 2, "Phase 2A.2/2B.1/2B.2: emitVye/runVye round-trip works for the Phase 3-5 subset." },
                { Feature::ArtifactCache,    "ArtifactCache",    SupportLevel::Supported,    SupportLevel::Supported, 11, "Phase 2C.3: opt-in via --enable-cache; content-addressed, atomic, recoverable." },
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

        // ---- .vye serialization (Phase 2A.2 / 2B.1) ----------------------------

        constexpr uint8_t  VYE_MAGIC[4] = { 'V', 'Y', 'E', '\0' };
        constexpr uint16_t VYE_VERSION = 1;

        void writeU16(std::string& s, uint16_t v) {
            s.push_back((char)(v & 0xFF));
            s.push_back((char)((v >> 8) & 0xFF));
        }
        void writeU32(std::string& s, uint32_t v) {
            s.push_back((char)(v & 0xFF));
            s.push_back((char)((v >> 8) & 0xFF));
            s.push_back((char)((v >> 16) & 0xFF));
            s.push_back((char)((v >> 24) & 0xFF));
        }
        void writeI32(std::string& s, int32_t v) { writeU32(s, (uint32_t)v); }
        void writeStr(std::string& s, const std::string& x) {
            writeU32(s, (uint32_t)x.size());
            s.append(x);
        }

        bool readU16(const std::string& s, size_t& off, uint16_t& v) {
            if (off + 2 > s.size()) return false;
            v = (uint16_t)((uint8_t)s[off]) | ((uint16_t)((uint8_t)s[off + 1]) << 8);
            off += 2;
            return true;
        }
        bool readU32(const std::string& s, size_t& off, uint32_t& v) {
            if (off + 4 > s.size()) return false;
            v = ((uint32_t)(uint8_t)s[off])
                | ((uint32_t)(uint8_t)s[off + 1] << 8)
                | ((uint32_t)(uint8_t)s[off + 2] << 16)
                | ((uint32_t)(uint8_t)s[off + 3] << 24);
            off += 4;
            return true;
        }
        bool readStr(const std::string& s, size_t& off, std::string& x) {
            uint32_t n = 0;
            if (!readU32(s, off, n)) return false;
            if (off + n > s.size()) return false;
            x.assign(s, off, n);
            off += n;
            return true;
        }

        std::string serializeModule(const VirModule& m) {
            std::string body;

            // constants
            writeU32(body, (uint32_t)m.constants.size());
            for (const auto& v : m.constants) {
                writeU16(body, (uint16_t)v.kind);
                switch (v.kind) {
                case VirValueKind::None:  break;
                case VirValueKind::Bool:  writeU16(body, v.boolValue ? 1 : 0); break;
                case VirValueKind::Int:   writeI32(body, (int32_t)v.intValue); break;
                case VirValueKind::Float: {
                    uint64_t bits;
                    static_assert(sizeof(bits) == sizeof(v.floatValue), "double size");
                    std::memcpy(&bits, &v.floatValue, sizeof(bits));
                    writeU32(body, (uint32_t)(bits & 0xFFFFFFFFULL));
                    writeU32(body, (uint32_t)((bits >> 32) & 0xFFFFFFFFULL));
                    break;
                }
                case VirValueKind::Str:   writeStr(body, v.strValue); break;
                }
            }

            // functions
            writeU32(body, (uint32_t)m.functions.size());
            for (const auto& fn : m.functions) {
                writeStr(body, fn.name);
                writeU32(body, (uint32_t)fn.paramNames.size());
                for (size_t i = 0; i < fn.paramNames.size(); ++i) {
                    writeStr(body, fn.paramNames[i]);
                    writeU16(body, (uint16_t)fn.paramTypes[i].kind);
                }
                writeU16(body, (uint16_t)fn.returnType.kind);
                writeI32(body, fn.localCount);
                writeU32(body, (uint32_t)fn.blocks.size());
                for (const auto& blk : fn.blocks) {
                    writeStr(body, blk.label);
                    writeU32(body, (uint32_t)blk.instructions.size());
                    for (const auto& ins : blk.instructions) {
                        writeU16(body, (uint16_t)ins.opcode);
                        writeI32(body, ins.destReg);
                        writeI32(body, ins.operandReg);
                        writeI32(body, ins.lhsReg);
                        writeI32(body, ins.rhsReg);
                        writeI32(body, ins.constIndex);
                        writeI32(body, ins.localIndex);
                        writeI32(body, ins.funcIndex);
                        writeU16(body, (uint16_t)ins.builtin);
                        writeU32(body, (uint32_t)ins.argRegs.size());
                        for (int r : ins.argRegs) writeI32(body, r);
                        writeI32(body, ins.targetBlock);
                    }
                }
                writeI32(body, fn.entryBlock);
            }

            // header
            std::string out;
            out.append((const char*)VYE_MAGIC, 4);
            writeU16(out, VYE_VERSION);
            writeU32(out, (uint32_t)body.size());
            out.append(body);
            writeI32(out, m.entryFunction);
            writeStr(out, m.name);
            return out;
        }

        bool deserializeModule(const std::string& raw, VirModule& m, std::string& err) {
            if (raw.size() < 10) { err = "vye: file too small"; return false; }
            if (raw[0] != 'V' || raw[1] != 'Y' || raw[2] != 'E' || raw[3] != '\0') {
                err = "vye: bad magic"; return false;
            }
            size_t off = 4;
            uint16_t version = 0;
            if (!readU16(raw, off, version)) { err = "vye: truncated version"; return false; }
            if (version != VYE_VERSION) { err = "vye: unsupported version " + std::to_string(version); return false; }
            uint32_t bodyLen = 0;
            if (!readU32(raw, off, bodyLen)) { err = "vye: truncated bodyLen"; return false; }
            if (off + bodyLen + 4 > raw.size()) { err = "vye: bodyLen past EOF"; return false; }
            size_t bodyEnd = off + bodyLen;

            uint32_t nconst = 0;
            if (!readU32(raw, off, nconst)) { err = "vye: truncated constants count"; return false; }
            if (nconst > 1000000) { err = "vye: constants count unreasonable"; return false; }
            m.constants.clear();
            m.constants.reserve(nconst);
            for (uint32_t i = 0; i < nconst; ++i) {
                VirValue v;
                uint16_t k = 0;
                if (!readU16(raw, off, k)) { err = "vye: truncated const kind"; return false; }
                v.kind = (VirValueKind)k;
                switch (v.kind) {
                case VirValueKind::None: break;
                case VirValueKind::Bool: {
                    uint16_t b = 0;
                    if (!readU16(raw, off, b)) { err = "vye: truncated bool const"; return false; }
                    v.boolValue = (b != 0);
                    break;
                }
                case VirValueKind::Int: {
                    uint32_t u = 0;
                    if (!readU32(raw, off, u)) { err = "vye: truncated int const"; return false; }
                    v.intValue = (int32_t)u;
                    break;
                }
                case VirValueKind::Float: {
                    uint32_t lo = 0, hi = 0;
                    if (!readU32(raw, off, lo) || !readU32(raw, off, hi)) { err = "vye: truncated float const"; return false; }
                    uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
                    std::memcpy(&v.floatValue, &bits, sizeof(bits));
                    break;
                }
                case VirValueKind::Str: {
                    if (!readStr(raw, off, v.strValue)) { err = "vye: truncated str const"; return false; }
                    break;
                }
                default: { err = "vye: unknown const kind " + std::to_string((int)v.kind); return false; }
                }
                m.constants.push_back(std::move(v));
            }

            uint32_t nfun = 0;
            if (!readU32(raw, off, nfun)) { err = "vye: truncated function count"; return false; }
            if (nfun > 100000) { err = "vye: function count unreasonable"; return false; }
            m.functions.clear();
            m.functions.reserve(nfun);
            for (uint32_t fi = 0; fi < nfun; ++fi) {
                VirFunction fn;
                if (!readStr(raw, off, fn.name)) { err = "vye: truncated function name"; return false; }
                uint32_t nparams = 0;
                if (!readU32(raw, off, nparams)) { err = "vye: truncated param count"; return false; }
                if (nparams > 100000) { err = "vye: param count unreasonable"; return false; }
                for (uint32_t pi = 0; pi < nparams; ++pi) {
                    std::string pname;
                    uint16_t pkind = 0;
                    if (!readStr(raw, off, pname) || !readU16(raw, off, pkind)) {
                        err = "vye: truncated param"; return false;
                    }
                    fn.paramNames.push_back(std::move(pname));
                    VirType t; t.kind = (VirTypeKind)pkind;
                    fn.paramTypes.push_back(t);
                }
                uint16_t rkind = 0;
                if (!readU16(raw, off, rkind)) { err = "vye: truncated return type"; return false; }
                fn.returnType.kind = (VirTypeKind)rkind;
                uint32_t lc = 0;
                if (!readU32(raw, off, lc)) { err = "vye: truncated localCount"; return false; }
                fn.localCount = (int)lc;
                uint32_t nblocks = 0;
                if (!readU32(raw, off, nblocks)) { err = "vye: truncated block count"; return false; }
                if (nblocks > 100000) { err = "vye: block count unreasonable"; return false; }
                for (uint32_t bi = 0; bi < nblocks; ++bi) {
                    VirBlock blk;
                    if (!readStr(raw, off, blk.label)) { err = "vye: truncated block label"; return false; }
                    uint32_t nins = 0;
                    if (!readU32(raw, off, nins)) { err = "vye: truncated instruction count"; return false; }
                    if (nins > 1000000) { err = "vye: instruction count unreasonable"; return false; }
                    for (uint32_t ii = 0; ii < nins; ++ii) {
                        VirInstruction ins;
                        uint16_t op = 0;
                        uint32_t d, o, l, r, c, li, fni;
                        uint16_t bi2 = 0;
                        if (!readU16(raw, off, op) ||
                            !readU32(raw, off, d) || !readU32(raw, off, o) ||
                            !readU32(raw, off, l) || !readU32(raw, off, r) ||
                            !readU32(raw, off, c) || !readU32(raw, off, li) ||
                            !readU32(raw, off, fni) || !readU16(raw, off, bi2)) {
                            err = "vye: truncated instruction"; return false;
                        }
                        ins.opcode = (VirOpcode)op;
                        ins.destReg = (int)d;
                        ins.operandReg = (int)o;
                        ins.lhsReg = (int)l;
                        ins.rhsReg = (int)r;
                        ins.constIndex = (int)c;
                        ins.localIndex = (int)li;
                        ins.funcIndex = (int)fni;
                        ins.builtin = (VirBuiltin)bi2;
                        uint32_t nargs = 0;
                        if (!readU32(raw, off, nargs)) { err = "vye: truncated argRegs count"; return false; }
                        if (nargs > 1000) { err = "vye: argRegs count unreasonable"; return false; }
                        ins.argRegs.resize(nargs);
                        for (uint32_t ai = 0; ai < nargs; ++ai) {
                            uint32_t ar = 0;
                            if (!readU32(raw, off, ar)) { err = "vye: truncated argReg"; return false; }
                            ins.argRegs[ai] = (int)ar;
                        }
                        uint32_t tb = 0;
                        if (!readU32(raw, off, tb)) { err = "vye: truncated targetBlock"; return false; }
                        ins.targetBlock = (int)tb;
                        blk.instructions.push_back(std::move(ins));
                    }
                    fn.blocks.push_back(std::move(blk));
                }
                uint32_t eb = 0;
                if (!readU32(raw, off, eb)) { err = "vye: truncated entryBlock"; return false; }
                fn.entryBlock = (int)eb;
                m.functions.push_back(std::move(fn));
            }

            if (off != bodyEnd) { err = "vye: body length mismatch"; return false; }

            uint32_t efi = 0;
            if (!readU32(raw, off, efi)) { err = "vye: truncated entryFunction"; return false; }
            m.entryFunction = (int)efi;
            if (!readStr(raw, off, m.name)) { err = "vye: truncated module name"; return false; }
            return true;
        }

        // ---- FNV-1a 64-bit for cache keys --------------------------------------
        uint64_t fnv1a(const std::string& s) {
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
            return h;
        }
        std::string toHex(uint64_t v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
            return buf;
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

        VirLowering lowering;
        LoweringResult lr = lowering.lower(program, sourceDir);
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

        // Phase 2C.3: populate content-addressed cache on successful lower+verify.
        if (cache_.enabled) {
            std::string serialized = serializeModule(lr.module);
            std::string dir = cache_.cacheDir.empty() ? ".vayu-cache" : cache_.cacheDir;
            std::string key = toHex(fnv1a(serialized));
            std::string sub = dir + "/objects";
#ifdef _WIN32
            system(("if not exist \"" + sub + "\" mkdir \"" + sub + "\"").c_str());
#else
            system(("mkdir -p \"" + sub + "\"").c_str());
#endif
            std::string objPath = sub + "/" + key + ".vye";
            std::ofstream cout(objPath, std::ios::binary);
            if (cout) {
                cout.write(serialized.data(), (std::streamsize)serialized.size());
                cout.close();
                if (cache_.verbose) {
                    std::fprintf(stderr, "vib: cache wrote %s (%zu bytes)\n",
                        objPath.c_str(), serialized.size());
                }
            }
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
        LoweringResult lr = lowering.lower(program, sourceDir);
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

    EmitVyeResult Integrator::emitVye(const vayu::Block& program,
        const std::string&,
        const std::string& vyePath) const {
        EmitVyeResult r;
        VirLowering lowering;
        LoweringResult lr = lowering.lower(program, sourceDir);
        if (!lr.success) {
            r.success = false;
            r.kind = ErrorKind::CompilationError;
            r.message = lr.message;
            return r;
        }
        VirVerifier verifier;
        VerifierResult vr = verifier.verify(lr.module);
        if (!vr.success) {
            r.success = false;
            r.kind = ErrorKind::VerifierError;
            r.message = vr.message;
            return r;
        }

        std::string serialized = serializeModule(lr.module);

        // Cache-aware write.
        if (cache_.enabled) {
            std::string dir = cache_.cacheDir.empty() ? ".vayu-cache" : cache_.cacheDir;
            std::string key = toHex(fnv1a(serialized));
            std::string sub = dir + "/objects";
#ifdef _WIN32
            system(("if not exist \"" + sub + "\" mkdir \"" + sub + "\"").c_str());
#else
            system(("mkdir -p \"" + sub + "\"").c_str());
#endif
            std::string objPath = sub + "/" + key + ".vye";
            std::ofstream out(objPath, std::ios::binary);
            if (out) {
                out.write(serialized.data(), (std::streamsize)serialized.size());
                out.close();
                if (cache_.verbose) {
                    std::fprintf(stderr, "vib: cache wrote %s (%zu bytes)\n",
                        objPath.c_str(), serialized.size());
                }
            }
        }

        std::ofstream out(vyePath, std::ios::binary);
        if (!out) {
            r.success = false;
            r.kind = ErrorKind::ArtifactError;
            r.message = "vib: cannot open \"" + vyePath + "\" for writing";
            return r;
        }
        out.write(serialized.data(), (std::streamsize)serialized.size());
        out.close();

        r.success = true;
        r.kind = ErrorKind::None;
        r.artifactPath = vyePath;
        r.bytesWritten = serialized.size();
        return r;
    }

    RunVyeResult Integrator::runVye(const std::string& vyePath) const {
        RunVyeResult r;

        std::ifstream in(vyePath, std::ios::binary);
        if (!in) {
            r.kind = ErrorKind::LoadingError;
            r.message = "vib: cannot open \"" + vyePath + "\"";
            return r;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string raw = ss.str();

        VirModule m;
        std::string err;
        if (!deserializeModule(raw, m, err)) {
            r.kind = ErrorKind::LoadingError;
            r.message = err;
            return r;
        }
        VirVerifier verifier;
        VerifierResult vr = verifier.verify(m);
        if (!vr.success) {
            r.kind = ErrorKind::VerifierError;
            r.message = vr.message;
            return r;
        }
        VirInterpreter interp;
        VirExecResult er = interp.execute(m);
        r.success = er.success;
        r.kind = er.kind;
        r.message = er.message;
        r.exitCode = er.exitCode;
        r.stdoutLines = std::move(er.stdoutLines);
        return r;
    }

} // namespace vib