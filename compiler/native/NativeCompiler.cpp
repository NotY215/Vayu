#include "NativeCompiler.hpp"
#include "VcbLower.hpp"
#include "parser/Parser.hpp"
#include "lexer/Lexer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#  define _CRT_NONSTDC_NO_DEPRECATE
#  include <process.h>
#  define VAYU_GETPID() _getpid()
#else
#  include <unistd.h>
#  define VAYU_GETPID() getpid()
#endif

namespace vayu {

    namespace {
        void tryRemove(const std::string& p) { std::remove(p.c_str()); }
    }

    NativeCompiler::NativeCompiler() {
#ifdef _WIN32
        vcbPath_ = "tools\\vcb.exe";
#else
        vcbPath_ = "tools/vcb";
#endif
        if (const char* p = std::getenv("VAYU_VCB")) {
            std::string s = p;
            if (s.find('\\') != std::string::npos ||
                s.find('/') != std::string::npos) {
                vcbPath_ = s;
            }
        }
    }

    std::string NativeCompiler::resolveVcbPath() const {
        auto exists = [](const std::string& p) -> bool {
            std::ifstream f(p, std::ios::binary);
            return (bool)f;
            };
#ifdef _WIN32
        const char* exeName = "vcb.exe";
        const char* subDir = "tools\\";
        const char* upUnit = "..\\";
#else
        const char* exeName = "vcb";
        const char* subDir = "tools/";
        const char* upUnit = "../";
#endif
        if (!vcbPath_.empty()) {
            bool hasSep = vcbPath_.find('\\') != std::string::npos ||
                vcbPath_.find('/') != std::string::npos;
            if (hasSep && exists(vcbPath_)) return vcbPath_;
        }
        {
            std::string cur = std::string(subDir) + exeName;
            if (exists(cur)) return cur;
        }
        for (int up = 1; up <= 6; ++up) {
            std::string prefix;
            for (int k = 0; k < up; ++k) prefix += upUnit;
            std::string cur = prefix + subDir + exeName;
            if (exists(cur)) return cur;
        }
        return {};
    }

    int NativeCompiler::compileAndRun(const Block& program,
        const std::string& sourceDir) {
        lastError_.clear();

        std::string ir;
        try {
            VcbLower lower;
            ir = lower.lower(program, sourceDir);
        }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "vcb-lower: %s\n", e.what());
            return 1;
        }

        std::error_code ec;
        std::filesystem::create_directories("_vayu_tmp", ec);
#ifdef _WIN32
        std::string base = "_vayu_tmp\\_vayu_" + std::to_string(VAYU_GETPID());
#else
        std::string base = "_vayu_tmp/_vayu_" + std::to_string(VAYU_GETPID());
#endif
        std::string irPath = base + ".vcbir";
        std::string exePath = outputExe_.empty() ? (base + ".exe") : outputExe_;
        const bool  compileOnly = !outputExe_.empty();

        {
            std::ofstream out(irPath, std::ios::binary);
            if (!out) { lastError_ = "cannot write " + irPath; return 1; }
            out << ir;
        }

        std::string vcbExe = resolveVcbPath();
        if (vcbExe.empty()) {
            lastError_ = "cannot find vcb.exe.  Place it at tools\\vcb.exe "
                "relative to the project root, or set VAYU_VCB "
                "to a path containing '\\' or '/'.";
            std::fprintf(stderr, "native: %s\n", lastError_.c_str());
            return 1;
        }

        // cmd.exe /c strips the first and last quote when the command
        // line starts with a quote and contains more than two.  Since
        // vcbExe is a project-relative path that never contains
        // spaces, leave it unquoted.  If a caller sets VAYU_VCB to a
        // spaced path, prefix with `call ` so cmd.exe never sees a
        // leading quote.
        std::string exeCmd;
        if (vcbExe.find(' ') == std::string::npos)
            exeCmd = vcbExe;
        else
            exeCmd = "call \"" + vcbExe + "\"";

        std::string cmd = exeCmd + " build \"" + irPath +
            "\" -o \"" + exePath + "\"";
        std::fprintf(stderr, "native: running %s\n", cmd.c_str());
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            lastError_ = "vcb failed (exit " + std::to_string(rc) +
                "). IR at " + irPath;
            std::fprintf(stderr, "native: %s\n", lastError_.c_str());
            return 1;
        }

        tryRemove(irPath);
        if (compileOnly) return 0;

        int runRc = std::system(("\"" + exePath + "\"").c_str());
        bool keep = (std::getenv("VAYU_KEEP_TEMP") != nullptr);
        if (!keep) tryRemove(exePath);

#ifdef _WIN32
        return runRc;
#else
        if (runRc == -1) {
            lastError_ = "failed to launch compiled executable";
            return 1;
        }
        if (WIFEXITED(runRc)) return WEXITSTATUS(runRc);
        return 128 + WTERMSIG(runRc);
#endif
    }

    void NativeCompiler::dumpIR(const Block& program,
        const std::string& sourceDir) {
        lastError_.clear();
        try {
            VcbLower lower;
            std::string ir = lower.lower(program, sourceDir);
            std::fwrite(ir.data(), 1, ir.size(), stdout);
        }
        catch (const std::exception& e) {
            lastError_ = e.what();
            std::fprintf(stderr, "vcb-lower: %s\n", e.what());
        }
    }

    bool NativeCompiler::writeRuntimeC(const std::string& path) const {
        // VCB has its own runtime embedded in the code generator.
        // There is no separate C source file to write.
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out << "/* Vayu native runtime is embedded in VCB.\n"
            "   There is no separate C source file in VCB mode.\n"
            "   See E:\\VCB\\src\\x64\\Runtime.cpp for the actual\n"
            "   runtime emitters. */\n";
        return true;
    }

} // namespace vayu