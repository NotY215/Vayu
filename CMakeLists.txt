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

# VIB is the integrated compiler/runtime architecture.
add_subdirectory(compiler)

# Optional self-host target (requires --native-out). Uses a distinct output name
# so it never clashes with the bootstrap bin/vayu.exe produced by compiler/.
option(VAYU_BUILD_SELFHOST_EXE "Build self-host vayu_selfhost.exe via --native-out" OFF)
if(WIN32 AND VAYU_BUILD_SELFHOST_EXE)
    find_program(VAYU_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(VAYU_POWERSHELL_EXECUTABLE)
        set(VAYU_SOURCE_FILE "${PROJECT_SOURCE_DIR}/vayu-src/vayu.vyu")
        set(VAYU_ICON_FILE "${PROJECT_SOURCE_DIR}/assets/Vayu_logo.ico")
        set(VAYU_ICON_SCRIPT "${PROJECT_SOURCE_DIR}/tools/apply_vayu_icon.ps1")
        set(VAYU_SELFHOST_EXE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/vayu_selfhost.exe")
        add_custom_command(
            OUTPUT "${VAYU_SELFHOST_EXE}"
            COMMAND "$<TARGET_FILE:vayuc>" "${VAYU_SOURCE_FILE}" --native-out "${VAYU_SELFHOST_EXE}"
            COMMAND "${VAYU_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                    -File "${VAYU_ICON_SCRIPT}"
                    -Executable "${VAYU_SELFHOST_EXE}"
                    -Icon "${VAYU_ICON_FILE}"
            DEPENDS vayuc "${VAYU_SOURCE_FILE}" "${VAYU_ICON_FILE}" "${VAYU_ICON_SCRIPT}"
            COMMENT "Self-host vayu_selfhost.exe + icon"
            VERBATIM
        )
        add_custom_target(vayu_executable DEPENDS "${VAYU_SELFHOST_EXE}")
    endif()
endif()
