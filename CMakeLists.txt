cmake_minimum_required(VERSION 3.20)
project(Vayu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# VIB (Vayu Integration Backend) is the integrated compiler/runtime architecture.
# Native code-generation milestones are tracked in Roadmap.md.

add_subdirectory(compiler)

# Build the user-facing Vayu executable from Vayu source and embed its Windows icon.
# This target requires working --native-out support in vayuc.
if(WIN32)
    find_program(VAYU_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(NOT VAYU_POWERSHELL_EXECUTABLE)
        message(FATAL_ERROR "PowerShell is required to embed assets/Vayu_logo.ico into vayu.exe.")
    endif()

    set(VAYU_SOURCE_FILE "${PROJECT_SOURCE_DIR}/vayu-src/vayu.vyu")
    set(VAYU_ICON_FILE "${PROJECT_SOURCE_DIR}/assets/Vayu_logo.ico")
    set(VAYU_ICON_SCRIPT "${PROJECT_SOURCE_DIR}/tools/apply_vayu_icon.ps1")
    set(VAYU_GENERATED_EXE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/vayu.exe")

    add_custom_command(
        OUTPUT "${VAYU_GENERATED_EXE}"
        COMMAND "$<TARGET_FILE:vayuc>" "${VAYU_SOURCE_FILE}" --native-out "${VAYU_GENERATED_EXE}"
        COMMAND "${VAYU_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                -File "${VAYU_ICON_SCRIPT}"
                -Executable "${VAYU_GENERATED_EXE}"
                -Icon "${VAYU_ICON_FILE}"
        DEPENDS
            vayuc
            "${VAYU_SOURCE_FILE}"
            "${VAYU_ICON_FILE}"
            "${VAYU_ICON_SCRIPT}"
        COMMENT "Compiling vayu.exe and embedding Vayu_logo.ico"
        VERBATIM
    )

    add_custom_target(vayu_executable DEPENDS "${VAYU_GENERATED_EXE}")
endif()
