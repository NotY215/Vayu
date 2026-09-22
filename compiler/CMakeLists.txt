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
)

target_include_directories(vayu_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(vayuc driver/main.cpp)
target_link_libraries(vayuc PRIVATE vayu_compiler)

add_executable(vls
    lsp/main.cpp
    lsp/LspServer.cpp
)
target_link_libraries(vls PRIVATE vayu_compiler)