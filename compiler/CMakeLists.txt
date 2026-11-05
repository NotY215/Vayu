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
target_include_directories(vayu_compiler PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vayuc.rc")
    add_executable(vayuc driver/main.cpp vayuc.rc)
else()
    add_executable(vayuc driver/main.cpp)
endif()
target_link_libraries(vayuc PRIVATE vayu_compiler)

# User-facing name: vayu.exe  |  tests also need vayuc.exe (POST_BUILD copy only)
set_target_properties(vayuc PROPERTIES OUTPUT_NAME "vayu")
add_custom_command(TARGET vayuc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:vayuc>
            $<TARGET_FILE_DIR:vayuc>/vayuc.exe
    COMMENT "Copy vayu.exe -> vayuc.exe for tests"
)

add_executable(vls lsp/main.cpp lsp/LspServer.cpp)
target_link_libraries(vls PRIVATE vayu_compiler)
add_executable(vfmt fmt/main.cpp fmt/Formatter.cpp)
target_link_libraries(vfmt PRIVATE vayu_compiler)
add_executable(vlint lint/main.cpp lint/Linter.cpp)
target_link_libraries(vlint PRIVATE vayu_compiler)

# IDE: Qt6 only (no Win32 Forms UI)
find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(_vayu_ide_sources
    ../tools/vayu_ide_qt/main.cpp
    ../tools/vayu_ide_qt/vayu_ide.qrc
)
if(WIN32)
    list(APPEND _vayu_ide_sources ../tools/vayu_ide_qt/vayu_ide_qt.rc)
endif()
add_executable(Vayu-IDE WIN32 ${_vayu_ide_sources})
target_link_libraries(Vayu-IDE PRIVATE Qt6::Widgets)
set_target_properties(Vayu-IDE PROPERTIES OUTPUT_NAME "Vayu-IDE")
message(STATUS "Vayu-IDE: Qt6 GUI (no console) + IDE.ico")
