#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "ast/Ast.hpp"
#include "sema/TypeChecker.hpp"
#include "interp/Interpreter.hpp"
#include "compile/Compiler.hpp"
#include "bytecode/Optimizer.hpp"
#include "vm/VM.hpp"
#include "native/NativeCompiler.hpp"
#include "VIB/Integrator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <iostream>
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
        "  --native             VIB native compilation and execution\n"
        "  --native-out <path>  compile to native exe at <path>, do not run\n"
        "  --check              type-check only\n"
        "  --dump-tokens        print the lexer output\n"
        "  --dump-ast           print the parsed AST\n"
        "  --dump-bytecode      compile to bytecode and print disassembly\n"
        "  --dump-ir            print VIB IR (when implemented)\n"
        "  --emit-vye <path>    write a .vye package to <path>\n  --output <path>      alias for --emit-vye (Vayu Executable)\n"
        "  --run-vye <path>     execute a .vye package\n"
        "\n"
        "Flags:\n"
        "  --no-check           skip the type checker\n"
        "  --no-opt             disable bytecode optimizer (VM only)\n"
        "  --bench [N]          run the program N times (default 5)\n"
        "  --link=external      select VIB-managed external linking (reserved)\n"
        "  --link=embedded      reserved compatibility option; VIB owns link policy\n"
        "  --enable-cache       enable the Phase 2C.3 artifact cache\n"
        "  --cache-dir <path>   cache directory (default .vayu-cache)\n"
        "  --dispatch=integrator route --run/--vm through Integrator dispatch\n"
        "\n"
        "Utility:\n"
        "  --emit-runtime <path>  unavailable until VIB runtime emission is implemented\n"
        "  --vib-status           print VIB Integrator capability matrix\n");
}

static void printVibStatus() {
    using namespace vib;
    std::printf("VIB Integrator status\n");
    std::printf("=====================\n\n");
    std::printf("Execution routes:\n");
    std::printf("  Interpreter   --run       working (external to Integrator)\n");
    std::printf("  VM            --vm        working (external to Integrator)\n");
    std::printf("  Native        --native    Scaffolded (Phase 12)\n");
    std::printf("  Vye           --run-vye   Supported (Phases 2A-2C)\n\n");
    std::printf("Feature capabilities:\n");
    std::printf("  %-26s %-13s %-11s %-5s %s\n",
        "Feature", "Native", "Vye", "Phase", "Note");
    std::printf("  %-26s %-13s %-11s %-5s %s\n",
        "--------------------------", "-------------",
        "-----------", "-----", "----");
    for (const auto& c : Integrator::capabilities()) {
        std::printf("  %-26s %-13s %-11s %-5d %s\n",
            c.name,
            Integrator::supportLevelName(c.nativeLevel),
            Integrator::supportLevelName(c.vyeLevel),
            c.plannedPhase,
            c.note);
    }
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


static void printBanner() {
    std::printf(
        "Vayu Programming Language\n"
        "Copyright (c) Vayu Community and NotY215. All rights reserved.\n"
        "Type --help for commands. Syntax: https://github.com/NotY215/Vayu/blob/master/docs/syntax.md\n"
        "Website syntax: https://vayu.gt.tc\n\n");
}

static void printHelp() {
    printBanner();
    std::printf(
        "Commands / flags:\n"
        "  <file.vyu>              run with tree-walking interpreter (default)\n"
        "  <file.vyu> --vm         run on the bytecode VM\n"
        "  <file.vyu> --native     run via VIB Integrator (supported subset)\n"
        "  <file.vyu> --check      type-check only\n"
        "  --emit-vye <path>       write a .vye Vayu Executable package\n"
        "  --output <path>         same as --emit-vye (creates .vye)\n"
        "  --run-vye <path>        execute a .vye package\n"
        "  --dump-ir               print VIR\n"
        "  --dump-ast / --dump-tokens / --dump-bytecode\n"
        "  --dispatch=integrator   route through Integrator\n"
        "  --enable-cache [--cache-dir <dir>]\n"
        "  --vib-status            feature capability matrix\n"
        "  --help                  this message\n"
        "  (no args)               interactive one-line REPL\n"
        "\n"
        "Syntax reference:\n"
        "  https://github.com/NotY215/Vayu/blob/master/docs/syntax.md\n"
        "  https://vayu.gt.tc\n");
}

static int runRepl() {
    printBanner();
    std::printf("vayu> ");
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) { std::printf("vayu> "); continue; }
        if (line == "exit" || line == "quit") break;
        if (line == "--help" || line == "help") { printHelp(); std::printf("vayu> "); continue; }
        // Wrap expression-like lines in print if needed; otherwise treat as program body.
        std::string src = line + "\n";
        vayu::Lexer lexer(std::move(src));
        auto tokens = lexer.tokenize();
        vayu::Block program;
        try {
            vayu::Parser parser(std::move(tokens));
            program = parser.parseProgram();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "parse error: %s\n", e.what());
            std::printf("vayu> ");
            continue;
        }
        if (!program.stmts.empty()) {
            // Prefer Integrator for one-liners when possible; fall back to interpreter.
            vayu::NativeCompiler nc;
            int rc = nc.compileAndRun(program, "");
            if (rc != 0) {
                try {
                    vayu::Interpreter interp;
                    interp.run(program);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "error: %s\n", e.what());
                }
            }
        }
        std::printf("vayu> ");
    }
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) { return runRepl(); }

    if (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h")) {
        printHelp();
        return 0;
    }

    if (std::strcmp(argv[1], "--emit-runtime") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "vayuc: --emit-runtime requires a path\n");
            return 1;
        }
        vayu::NativeCompiler nc;
        if (!nc.writeRuntimeC(argv[2])) {
            std::fprintf(stderr, "vayuc: %s\n", nc.lastError().c_str());
            return 1;
        }
        return 0;
    }

    if (std::strcmp(argv[1], "--vib-status") == 0) {
        printVibStatus();
        return 0;
    }

    // --run-vye may be argv[1] with no source file (standalone mode).
    if (std::strcmp(argv[1], "--run-vye") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "vayuc: --run-vye requires a path\n");
            return 1;
        }
        vayu::NativeCompiler nc;
        int rc = nc.runVye(argv[2]);
        for (const auto& line : nc.lastStdout()) {
            std::fputs(line.c_str(), stdout);
            std::fputc('\n', stdout);
        }
        if (rc != 0) {
            std::fprintf(stderr, "vayuc: %s\n", nc.lastError().c_str());
        }
        return rc;
    }

    std::string file = argv[1];

    enum class Mode { Run, Check, DumpTokens, DumpAst, DumpBytecode, DumpIR, Native };
    // Convenience: `vayuc path/to/file.vye` runs the package (same as --run-vye).
    {
        std::string a1 = argv[1];
        auto dot = a1.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = a1.substr(dot);
            for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (ext == ".vye") {
                vayu::NativeCompiler nc;
                int rc = nc.runVye(argv[1]);
                for (const auto& line : nc.lastStdout()) {
                    std::fputs(line.c_str(), stdout);
                    if (line.empty() || line.back() != '\n') std::fputc('\n', stdout);
                }
                if (rc != 0 && !nc.lastError().empty())
                    std::fprintf(stderr, "vayuc: %s\n", nc.lastError().c_str());
                return rc;
            }
        }
    }

    Mode mode = Mode::Run;
    bool skipCheck = false;
    bool useVM = false;
    bool useOpt = true;
    int  benchRuns = 0;
    std::string nativeOutPath;
    vayu::NativeCompiler::LinkMode linkMode =
        vayu::NativeCompiler::LinkMode::External;
    std::string emitVyePath;
    std::string runVyePath;
    bool        runVye = false;
    bool        enableCache = false;
    std::string cacheDir;
    bool        dispatchIntegrator = false;

    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--run"))           mode = Mode::Run;
        else if (!std::strcmp(a, "--check"))         mode = Mode::Check;
        else if (!std::strcmp(a, "--dump-tokens"))   mode = Mode::DumpTokens;
        else if (!std::strcmp(a, "--dump-ast"))      mode = Mode::DumpAst;
        else if (!std::strcmp(a, "--dump-bytecode")) mode = Mode::DumpBytecode;
        else if (!std::strcmp(a, "--dump-ir"))       mode = Mode::DumpIR;
        else if (!std::strcmp(a, "--native"))        mode = Mode::Native;
        else if (!std::strcmp(a, "--link=embedded")) linkMode =
            vayu::NativeCompiler::LinkMode::Embedded;
        else if (!std::strcmp(a, "--link=external")) linkMode =
            vayu::NativeCompiler::LinkMode::External;
        else if (!std::strcmp(a, "--vm"))            useVM = true;
        else if (!std::strcmp(a, "--no-check"))      skipCheck = true;
        else if (!std::strcmp(a, "--no-opt"))        useOpt = false;
        else if (!std::strcmp(a, "--backend")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "vayuc: --backend requires an argument\n");
                return 1;
            }
            std::string b = argv[++i];
            if (b != "integrator" && b != "auto") {
                std::fprintf(stderr,
                    "vayuc: backend '%s' no longer exists; "
                    "VIB is the integrated native-toolchain architecture\n", b.c_str());
                return 1;
            }
        }
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
        else if (!std::strcmp(a, "--emit-vye") || !std::strcmp(a, "--output")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "vayuc: %s requires a path\n", a);
                return 1;
            }
            emitVyePath = argv[++i];
        }
        else if (!std::strcmp(a, "--run-vye")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "vayuc: --run-vye requires a path\n");
                return 1;
            }
            runVyePath = argv[++i];
            runVye = true;
        }
        else if (!std::strcmp(a, "--enable-cache")) {
            enableCache = true;
        }
        else if (!std::strcmp(a, "--cache-dir")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "vayuc: --cache-dir requires a path\n");
                return 1;
            }
            cacheDir = argv[++i];
        }
        else if (!std::strcmp(a, "--dispatch=integrator")) {
            dispatchIntegrator = true;
        }
        else {
            std::fprintf(stderr, "vayuc: unknown flag '%s'\n", a);
            usage();
            return 1;
        }
    }

    // --run-vye  (before source load; doesn't require .vyu)
    if (runVye) {
        vayu::NativeCompiler nc;
        nc.setCache(enableCache, cacheDir, false);
        int rc = nc.runVye(runVyePath);
        for (const auto& line : nc.lastStdout()) {
            std::fputs(line.c_str(), stdout);
            std::fputc('\n', stdout);
        }
        if (rc != 0) {
            std::fprintf(stderr, "vayuc: %s\n", nc.lastError().c_str());
        }
        return rc;
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

    // ---------- emit-vye ----------
    if (!emitVyePath.empty()) {
        vayu::NativeCompiler nc;
        nc.setCache(enableCache, cacheDir, false);
        int rc = nc.emitVye(program, srcDir, emitVyePath);
        if (rc != 0) {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), nc.lastError().c_str());
            return rc;
        }
        return 0;
    }

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
        std::string ir = nc.dumpIR(program, srcDir);
        if (!nc.lastError().empty()) {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), nc.lastError().c_str());
            return 1;
        }
        std::fputs(ir.c_str(), stdout);
        return 0;
    }

    // ---------- native ----------
    if (mode == Mode::Native || dispatchIntegrator) {
        if (!nativeOutPath.empty()) {
            vayu::NativeCompiler nc;
            nc.setCache(enableCache, cacheDir, false);
            nc.setLinkMode(linkMode);
            nc.setOutputExe(nativeOutPath);
            int rc = nc.compileAndRun(program, srcDir);
            if (rc != 0) {
                std::fprintf(stderr, "%s: %s\n", file.c_str(), nc.lastError().c_str());
            }
            return rc;
        }

        if (benchRuns > 0) {
            runBenchmark("native (includes compile)", benchRuns, [&]() {
                vayu::NativeCompiler nc;
                nc.setCache(enableCache, cacheDir, false);
                nc.setLinkMode(linkMode);
                if (nc.compileAndRun(program, srcDir) != 0) {
                    std::fprintf(stderr, "%s: %s\n",
                        file.c_str(), nc.lastError().c_str());
                    std::exit(2);
                }
                });
            return 0;
        }

        vayu::NativeCompiler nc;
        nc.setCache(enableCache, cacheDir, false);
        nc.setLinkMode(linkMode);
        int rc = nc.compileAndRun(program, srcDir);
        if (rc != 0) {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), nc.lastError().c_str());
        }
        return rc;
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