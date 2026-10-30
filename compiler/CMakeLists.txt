add_library(vayu_compiler STATIC
    lexer/Token.cpp
    lexer/Lexer.cpp
    ast/Ast.cpp
    parser/Parser.cpp
    types/Type.cpp
    sema/TypeChecker.cpp
    interp/Value.cpp
    interp/Interpreter.cpp
    bytecode/OpCode.cpp
    bytecode/Chunk.cpp
    bytecode/Optimizer.cpp
    compile/Compiler.cpp
    vm/VM.cpp
    native/NativeCompiler.cpp
    ../VIB/Integrator.cpp
    ../VIB/Vir.cpp
    ../VIB/VirInterp.cpp
)
target_include_directories(vayu_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vayuc.rc")
    add_executable(vayuc driver/main.cpp vayuc.rc)
else()
    add_executable(vayuc driver/main.cpp)
endif()
target_link_libraries(vayuc PRIVATE vayu_compiler)
add_executable(vls lsp/main.cpp lsp/LspServer.cpp)
target_link_libraries(vls PRIVATE vayu_compiler)
add_executable(vfmt fmt/main.cpp fmt/Formatter.cpp)
target_link_libraries(vfmt PRIVATE vayu_compiler)
add_executable(vlint lint/main.cpp lint/Linter.cpp)
target_link_libraries(vlint PRIVATE vayu_compiler)

# Lightweight Vayu-IDE (Win32)
if(WIN32)
    add_executable(Vayu-IDE WIN32
        ../tools/vayu_ide/main.cpp
        ../tools/vayu_ide/vayu_ide.rc
    )
    target_link_libraries(Vayu-IDE PRIVATE comdlg32 user32 gdi32)
    set_target_properties(Vayu-IDE PROPERTIES OUTPUT_NAME "Vayu-IDE")
endif()
