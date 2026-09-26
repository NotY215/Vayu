#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "ast/Ast.hpp"
#include "sema/TypeChecker.hpp"
#include "interp/Interpreter.hpp"
#include "compile/Compiler.hpp"
#include "bytecode/Optimizer.hpp"
#include "vm/VM.hpp"
#include "native/NativeCompiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

static std::string sourceDirOf(const std::string& file) {
    auto slash = file.find_last_of("/\\");
    if (slash == std::string::npos) return {};
    return file.substr(0, slash + 1);
}

static void usage() {
    std::fprintf(stderr,
        "usage: vayuc <file.vyu> [mode] [flags]\n"
        "\n"
        "Modes (default --run):\n"
        "  --run                tree-walking interpreter\n"
        "  --vm                 bytecode virtual machine\n"
        "  --native             QBE native compilation and execution\n"
        "  --backend <qbe|vcb>  native backend (default qbe)\n"
        "  --native-out <path>  compile to native exe at <path>, do not run\n"
        "  --check              type-check only\n"
        "  --dump-tokens        print the lexer output\n"
        "  --dump-ast           print the parsed AST\n"
        "  --dump-bytecode      compile to bytecode and print disassembly\n"
        "  --dump-ir            print the QBE IL\n"
        "\n"
        "Flags:\n"
        "  --opt <0-3>          gcc optimization level (default 2)\n"
        "  --no-check           skip the type checker\n"
        "  --no-opt             disable bytecode optimizer (VM only)\n"
        "  --bench [N]          run the program N times (default 5)\n"
        "\n"
        "Utility:\n"
        "  --emit-runtime <path>  write the native runtime C source to <path>\n");
}

namespace {
    template <typename RunFn>
    void runBenchmark(const char* label, int runs, RunFn&& oneRun) {
        std::vector<double> times;
        times.reserve((size_t)runs);
        for (int r = 0; r < runs; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            oneRun();
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            times.push_back(ms);
            std::fprintf(stderr, "run %d: %.3f ms\n", r + 1, ms);
        }
        double best = *std::min_element(times.begin(), times.end());
        double avg = 0.0;
        for (double t : times) avg += t;
        avg /= (double)times.size();
        std::fprintf(stderr,
            "[%s] best: %.3f ms   avg: %.3f ms   runs: %d\n",
            label, best, avg, runs);
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) { usage(); return 1; }

    // --emit-runtime <path>  (handled before any other arg parsing)
    if (std::strcmp(argv[1], "--emit-runtime") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "vayuc: --emit-runtime requires a path\n");
            return 1;
        }
        vayu::NativeCompiler nc;
        if (!nc.writeRuntimeC(argv[2])) {
            std::fprintf(stderr, "vayuc: cannot write runtime to '%s'\n", argv[2]);
            return 1;
        }
        return 0;
    }

    // Pre-scan for --opt <n>.  Removed from the argument list so the rest of
    // main can keep treating argv[1] as the input file regardless of where
    // --opt appeared.
    int g_optLevel = 2;
    std::vector<char*> nargv;
    nargv.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--opt") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "vayuc: --opt requires a level (0-3)\n");
                return 1;
            }
            char* end = nullptr;
            long v = std::strtol(argv[i + 1], &end, 10);
            if (!end || *end != '\0' || v < 0 || v > 3) {
                std::fprintf(stderr, "vayuc: --opt level must be 0, 1, 2, or 3\n");
                return 1;
            }
            g_optLevel = (int)v;
            ++i;
            continue;
        }
        nargv.push_back(argv[i]);
    }
    argc = (int)nargv.size();
    argv = nargv.data();

    if (argc < 2) { usage(); return 1; }

    std::string file = argv[1];

    enum class Mode { Run, Check, DumpTokens, DumpAst, DumpBytecode, DumpIR, Native };
    Mode mode = Mode::Run;
    bool skipCheck = false;
    bool useVM = false;
    bool useOpt = true;
    int  benchRuns = 0;
    std::string nativeOutPath;
    vayu::NativeBackend g_backend = vayu::NativeBackend::Qbe;

    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--run"))           mode = Mode::Run;
        else if (!std::strcmp(a, "--check"))         mode = Mode::Check;
        else if (!std::strcmp(a, "--dump-tokens"))   mode = Mode::DumpTokens;
        else if (!std::strcmp(a, "--dump-ast"))      mode = Mode::DumpAst;
        else if (!std::strcmp(a, "--dump-bytecode")) mode = Mode::DumpBytecode;
        else if (!std::strcmp(a, "--dump-ir"))       mode = Mode::DumpIR;
        else if (!std::strcmp(a, "--native"))        mode = Mode::Native;
        else if (!std::strcmp(a, "--backend")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "vayuc: --backend requires qbe or vcb\n");
                return 1;
            }
            std::string b = argv[++i];
            if (b == "vcb")      g_backend = vayu::NativeBackend::Vcb;
            else if (b == "qbe") g_backend = vayu::NativeBackend::Qbe;
            else {
                std::fprintf(stderr, "vayuc: unknown backend '%s'\n", b.c_str());
                return 1;
            }
        }
        else if (!std::strcmp(a, "--vm"))            useVM = true;
        else if (!std::strcmp(a, "--no-check"))      skipCheck = true;
        else if (!std::strcmp(a, "--no-opt"))        useOpt = false;
        else if (!std::strcmp(a, "--native-out")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "vayuc: --native-out requires a path\n");
                return 1;
            }
            nativeOutPath = argv[++i];
            mode = Mode::Native;
        }
        else if (!std::strcmp(a, "--bench")) {
            benchRuns = 5;
            if (i + 1 < argc) {
                char* end = nullptr;
                long v = std::strtol(argv[i + 1], &end, 10);
                if (end && *end == '\0' && v > 0) { benchRuns = (int)v; ++i; }
            }
        }
        else {
            std::fprintf(stderr, "vayuc: unknown flag '%s'\n", a);
            usage();
            return 1;
        }
    }

    std::string src = readFile(file);
    if (src.empty()) {
        std::fprintf(stderr, "vayuc: cannot read '%s'\n", file.c_str());
        return 1;
    }

    vayu::Lexer lexer(std::move(src));
    auto tokens = lexer.tokenize();

    if (mode == Mode::DumpTokens) {
        for (const auto& t : tokens)
            std::printf("%3d:%-3d  %-14s  %s\n",
                t.location.line, t.location.column,
                vayu::tokenTypeName(t.type), t.lexeme.c_str());
        return 0;
    }

    vayu::Block program;
    try {
        vayu::Parser parser(std::move(tokens));
        program = parser.parseProgram();
    }
    catch (const vayu::ParseError& e) {
        std::fprintf(stderr, "%s:%d:%d: parse error: %s\n",
            file.c_str(), e.loc.line, e.loc.column, e.what());
        return 1;
    }

    if (mode == Mode::DumpAst) { vayu::printProgram(program); return 0; }

    if (!skipCheck && mode != Mode::DumpBytecode && mode != Mode::DumpIR) {
        try {
            vayu::TypeChecker checker;
            checker.check(program);
        }
        catch (const vayu::TypeError& e) {
            std::fprintf(stderr, "%s:%d:%d: type error: %s\n",
                file.c_str(), e.loc.line, e.loc.column, e.what());
            return 1;
        }
    }

    if (mode == Mode::Check) {
        std::printf("OK: %s type-checks successfully.\n", file.c_str());
        return 0;
    }

    std::string srcDir = sourceDirOf(file);

    // ---------- dump-bytecode ----------
    if (mode == Mode::DumpBytecode) {
        auto chunk = std::make_shared<vayu::Chunk>();
        try {
            vayu::Compiler c; c.compile(program, *chunk);
        }
        catch (const vayu::CompileError& e) {
            std::fprintf(stderr, "%s:%d:%d: compile error: %s\n",
                file.c_str(), e.loc.line, e.loc.column, e.what());
            return 1;
        }
        std::printf("--- raw bytecode ---\n");
        vayu::disassemble(*chunk, file.c_str());
        if (useOpt) {
            vayu::OptStats stats;
            vayu::optimizeChunk(*chunk, stats);
            std::printf("--- after optimization (folded=%d) ---\n",
                stats.constantsFolded);
            vayu::disassemble(*chunk, file.c_str());
        }
        return 0;
    }

    // ---------- dump-ir ----------
    if (mode == Mode::DumpIR) {
        vayu::NativeCompiler nc;
        nc.setOptLevel(g_optLevel);
        nc.setBackend(g_backend);
        nc.dumpIR(program, srcDir);
        return nc.lastError().empty() ? 0 : 1;
    }

    // ---------- native ----------
    if (mode == Mode::Native) {
        if (!nativeOutPath.empty()) {
            vayu::NativeCompiler nc;
            nc.setOptLevel(g_optLevel);
            nc.setBackend(g_backend);
            nc.setOutputExe(nativeOutPath);
            int rc = nc.compileAndRun(program, srcDir);
            if (rc != 0) {
                std::fprintf(stderr, "%s: native error: %s\n",
                    file.c_str(), nc.lastError().c_str());
            }
            return rc;
        }

        if (benchRuns > 0) {
            runBenchmark("native (includes compile)", benchRuns, [&]() {
                vayu::NativeCompiler nc;
                nc.setOptLevel(g_optLevel);
                nc.setBackend(g_backend);
                if (nc.compileAndRun(program, srcDir) != 0) {
                    std::fprintf(stderr, "native: %s\n",
                        nc.lastError().c_str());
                    std::exit(2);
                }
                });
            return 0;
        }

        vayu::NativeCompiler nc;
        nc.setOptLevel(g_optLevel);
        nc.setBackend(g_backend);
        return nc.compileAndRun(program, srcDir);
    }

    // ---------- vm ----------
    if (useVM) {
        auto chunk = std::make_shared<vayu::Chunk>();
        try {
            vayu::Compiler c; c.compile(program, *chunk);
        }
        catch (const vayu::CompileError& e) {
            std::fprintf(stderr, "%s:%d:%d: VM compile error: %s\n",
                file.c_str(), e.loc.line, e.loc.column, e.what());
            return 1;
        }
        if (useOpt) {
            vayu::OptStats stats;
            vayu::optimizeChunk(*chunk, stats);
        }
        if (benchRuns > 0) {
            runBenchmark(useOpt ? "vm-opt" : "vm-no-opt", benchRuns, [&]() {
                vayu::Interpreter interp;
                interp.setSourceDir(srcDir);
                interp.registerDeclarations(program);
                vayu::VM vm(interp.globals());
                vm.run(chunk);
                });
            return 0;
        }
        try {
            vayu::Interpreter interp;
            interp.setSourceDir(srcDir);
            interp.registerDeclarations(program);
            vayu::VM vm(interp.globals());
            vm.run(chunk);
        }
        catch (const vayu::VayuException& e) {
            std::fprintf(stderr, "%s:%d:%d: uncaught %s: %s\n",
                file.c_str(), e.loc.line, e.loc.column,
                vayu::Interpreter::exceptionTypeName(e.value).c_str(),
                vayu::Interpreter::exceptionMessage(e.value).c_str());
            return 1;
        }
        catch (const vayu::VMRuntimeError& e) {
            std::fprintf(stderr, "%s:%d: VM runtime error: %s\n",
                file.c_str(), e.line, e.what());
            return 1;
        }
        catch (const vayu::RuntimeError& e) {
            std::fprintf(stderr, "%s:%d:%d: runtime error: %s\n",
                file.c_str(), e.loc.line, e.loc.column, e.what());
            return 1;
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "%s: VM error: %s\n", file.c_str(), e.what());
            return 1;
        }
        return 0;
    }

    // ---------- tree-walk ----------
    if (benchRuns > 0) {
        runBenchmark("tree-walk", benchRuns, [&]() {
            vayu::Interpreter interp;
            interp.setSourceDir(srcDir);
            interp.run(program);
            });
        return 0;
    }
    try {
        vayu::Interpreter interp;
        interp.setSourceDir(srcDir);
        interp.run(program);
    }
    catch (const vayu::VayuException& e) {
        std::fprintf(stderr, "%s:%d:%d: uncaught %s: %s\n",
            file.c_str(), e.loc.line, e.loc.column,
            vayu::Interpreter::exceptionTypeName(e.value).c_str(),
            vayu::Interpreter::exceptionMessage(e.value).c_str());
        return 1;
    }
    catch (const vayu::RuntimeError& e) {
        std::fprintf(stderr, "%s:%d:%d: runtime error: %s\n",
            file.c_str(), e.loc.line, e.loc.column, e.what());
        return 1;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%s: runtime error: %s\n", file.c_str(), e.what());
        return 1;
    }
    return 0;
}