# SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
# Copyright (c) 2026 KenTread. All Rights Reserved.
# Proprietary and confidential -- see LICENSE for terms.
#
# Top-level build for OmniFractal.
#
# Every third-party dependency is optional and gated behind an
# OMF_WITH_* switch. The core builds and renders with none of them
# enabled -- that configuration is what CI compiles on every push, so a
# missing SDK on a contributor's machine can never break the build.

cmake_minimum_required(VERSION 3.25)

project(OmniFractal
    VERSION 0.1.0
    DESCRIPTION "OmniFractal -- 64-bit GPU fractal renderer with Mandelbulb 3D format import"
    LANGUAGES CXX C)

# ---------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------

option(OMF_WITH_VULKAN       "Vulkan 1.3 compute backend (volk + VMA)" ON)
option(OMF_WITH_LUA          "LuaJIT scripting via sol2"               ON)
option(OMF_WITH_TINYEXR      "Multi-layer OpenEXR export"              ON)
# OCCT is LGPL-2.1 + exception. It is DYNAMICALLY linked, always. See the
# enforcement block below and THIRD-PARTY-NOTICES.md.
option(OMF_WITH_OPENCASCADE  "STEP (ISO 10303) export via OCCT"        OFF)
option(OMF_WITH_MINIZ        "DEFLATE support for legacy import"       ON)
option(OMF_BUILD_FUZZERS     "Build libFuzzer parser harnesses"        OFF)
option(OMF_WITH_GRPC         "Distributed tile dispatcher"             OFF)
option(OMF_BUILD_TESTS       "Build the core test suite"               ON)
option(OMF_BUILD_CLI         "Build the headless omf-cli renderer"    ON)
option(OMF_FETCH_DEPS        "Fetch missing dependencies with FetchContent" ON)

# ---------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Symbols are opt-in: the ABI surface is exactly what OMF_API marks.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# 64-bit only. The whole point of v2 is escaping the 32-bit address space
# the Delphi original was stuck in, and a 32-bit build would silently
# reintroduce the allocation ceiling this project exists to remove.
if(CMAKE_SIZEOF_VOID_P LESS 8)
    message(FATAL_ERROR "OmniFractal requires a 64-bit toolchain (found ${CMAKE_SIZEOF_VOID_P}-byte pointers)")
endif()

# ---------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------

set(OMF_PRIVATE_LIBS "")
set(OMF_PRIVATE_DEFS "")
set(OMF_PRIVATE_INCLUDES "")

# -- GLM: header-only, used by the tooling and shader-side maths --------
find_package(glm QUIET)
if(NOT glm_FOUND AND OMF_FETCH_DEPS)
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(glm)
endif()
if(TARGET glm::glm)
    list(APPEND OMF_PRIVATE_LIBS glm::glm)
elseif(TARGET glm)
    list(APPEND OMF_PRIVATE_LIBS glm)
endif()

# -- Vulkan: volk loads entry points at runtime, VMA does allocation ----
if(OMF_WITH_VULKAN)
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(WARNING "Vulkan SDK not found -- building CPU-only. "
                        "Set OMF_WITH_VULKAN=OFF to silence this.")
        set(OMF_WITH_VULKAN OFF)
    endif()
endif()

if(OMF_WITH_VULKAN)
    if(OMF_FETCH_DEPS)
        FetchContent_Declare(volk
            GIT_REPOSITORY https://github.com/zeux/volk.git
            GIT_TAG        1.3.270
            GIT_SHALLOW    TRUE)
        FetchContent_Declare(vma
            GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
            GIT_TAG        v3.1.0
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(volk vma)
    endif()

    list(APPEND OMF_PRIVATE_DEFS OMF_WITH_VULKAN)
    # volk dlopens the loader itself; linking Vulkan::Headers only keeps
    # the binary loadable on machines with no Vulkan runtime at all.
    list(APPEND OMF_PRIVATE_LIBS Vulkan::Headers)
    if(TARGET volk::volk_headers)
        list(APPEND OMF_PRIVATE_LIBS volk::volk_headers)
    elseif(TARGET volk)
        list(APPEND OMF_PRIVATE_LIBS volk)
    endif()
    if(TARGET GPUOpen::VulkanMemoryAllocator)
        list(APPEND OMF_PRIVATE_LIBS GPUOpen::VulkanMemoryAllocator)
    endif()
endif()

# -- LuaJIT + sol2 ------------------------------------------------------
if(OMF_WITH_LUA)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LUAJIT IMPORTED_TARGET luajit)
    endif()

    if(NOT LUAJIT_FOUND)
        message(WARNING "LuaJIT not found -- scripting disabled. "
                        "Install luajit-dev or set OMF_WITH_LUA=OFF.")
        set(OMF_WITH_LUA OFF)
    else()
        if(OMF_FETCH_DEPS)
            FetchContent_Declare(sol2
                GIT_REPOSITORY https://github.com/ThePhD/sol2.git
                GIT_TAG        v3.3.0
                GIT_SHALLOW    TRUE)
            FetchContent_MakeAvailable(sol2)
        endif()
        list(APPEND OMF_PRIVATE_DEFS OMF_WITH_LUA)
        list(APPEND OMF_PRIVATE_LIBS PkgConfig::LUAJIT)
        if(TARGET sol2::sol2)
            list(APPEND OMF_PRIVATE_LIBS sol2::sol2)
        endif()
    endif()
endif()

# -- tinyexr ------------------------------------------------------------
if(OMF_WITH_TINYEXR)
    if(OMF_FETCH_DEPS)
        FetchContent_Declare(tinyexr
            GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
            GIT_TAG        v1.0.8
            GIT_SHALLOW    TRUE)
        FetchContent_Populate(tinyexr)
        list(APPEND OMF_PRIVATE_INCLUDES ${tinyexr_SOURCE_DIR})
        list(APPEND OMF_PRIVATE_DEFS OMF_WITH_TINYEXR)
    else()
        find_path(TINYEXR_INCLUDE_DIR tinyexr.h)
        if(TINYEXR_INCLUDE_DIR)
            list(APPEND OMF_PRIVATE_INCLUDES ${TINYEXR_INCLUDE_DIR})
            list(APPEND OMF_PRIVATE_DEFS OMF_WITH_TINYEXR)
        else()
            message(WARNING "tinyexr not found -- EXR export disabled")
            set(OMF_WITH_TINYEXR OFF)
        endif()
    endif()
endif()

# -- miniz: DEFLATE for the compressed legacy containers ---------------
if(OMF_WITH_MINIZ)
    if(OMF_FETCH_DEPS)
        FetchContent_Declare(miniz
            GIT_REPOSITORY https://github.com/richgel999/miniz.git
            GIT_TAG        3.0.2
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(miniz)
        list(APPEND OMF_PRIVATE_DEFS OMF_WITH_MINIZ)
        if(TARGET miniz)
            list(APPEND OMF_PRIVATE_LIBS miniz)
        endif()
    else()
        find_path(MINIZ_INCLUDE_DIR miniz.h)
        find_library(MINIZ_LIBRARY miniz)
        if(MINIZ_INCLUDE_DIR AND MINIZ_LIBRARY)
            list(APPEND OMF_PRIVATE_INCLUDES ${MINIZ_INCLUDE_DIR})
            list(APPEND OMF_PRIVATE_LIBS ${MINIZ_LIBRARY})
            list(APPEND OMF_PRIVATE_DEFS OMF_WITH_MINIZ)
        else()
            # Without DEFLATE the legacy containers cannot be opened at
            # all, so this is a hard failure rather than a quiet
            # degradation -- import is the product's reason to exist.
            message(FATAL_ERROR
                "miniz not found and OMF_FETCH_DEPS=OFF. Legacy import requires DEFLATE. "
                "Install miniz, enable OMF_FETCH_DEPS, or set OMF_WITH_MINIZ=OFF to "
                "build without legacy import.")
        endif()
    endif()
endif()

# -- OpenCASCADE --------------------------------------------------------
# Off by default: OCCT is a large SDK and only the STEP path needs it.
if(OMF_WITH_OPENCASCADE)
    find_package(OpenCASCADE QUIET)
    if(OpenCASCADE_FOUND)
        # LICENSING GATE, NOT A PREFERENCE.
        #
        # OCCT is LGPL-2.1 with an exception. LGPL section 6 permits
        # combining it with proprietary software only if the user retains
        # the ability to relink against a modified library. Static linking
        # removes that ability and would subject all of OmniFractal to the
        # LGPL's source and relink obligations.
        #
        # So a static OCCT fails configuration. There is no override flag
        # -- an override would exist only to violate the license.
        foreach(_occt_lib IN ITEMS TKernel TKSTEP TKBRep)
            if(TARGET ${_occt_lib})
                get_target_property(_occt_type ${_occt_lib} TYPE)
                if(_occt_type STREQUAL "STATIC_LIBRARY")
                    message(FATAL_ERROR
                        "Found a STATIC OpenCASCADE (${_occt_lib}). OCCT is LGPL-2.1 and "
                        "must be dynamically linked so users can exercise their relink "
                        "rights -- see LICENSE section 9.2 and THIRD-PARTY-NOTICES.md. "
                        "Install a shared OCCT build, or set OMF_WITH_OPENCASCADE=OFF "
                        "to build without STEP export.")
                endif()
            endif()
        endforeach()

        list(APPEND OMF_PRIVATE_DEFS OMF_WITH_OPENCASCADE)
        list(APPEND OMF_PRIVATE_INCLUDES ${OpenCASCADE_INCLUDE_DIR})
        list(APPEND OMF_PRIVATE_LIBS
            TKernel TKMath TKBRep TKTopAlgo TKPrim TKShHealing
            TKSTEP TKSTEPBase TKSTEPAttr TKXSBase TKG2d TKG3d TKGeomBase)
    else()
        message(WARNING "OpenCASCADE not found -- STEP export disabled")
        set(OMF_WITH_OPENCASCADE OFF)
    endif()
endif()

find_package(Threads REQUIRED)
list(APPEND OMF_PRIVATE_LIBS Threads::Threads)

# ---------------------------------------------------------------------
# Shader compilation
# ---------------------------------------------------------------------

set(OMF_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/shaders)

if(OMF_WITH_VULKAN)
    find_program(GLSLC_EXECUTABLE NAMES glslc glslangValidator
                 HINTS $ENV{VULKAN_SDK}/bin)
endif()

set(OMF_SPIRV_FILES "")
if(GLSLC_EXECUTABLE)
    file(MAKE_DIRECTORY ${OMF_SHADER_OUTPUT_DIR})
    foreach(shader raymarch.comp marching_cubes.comp)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/${shader})
        set(dst ${OMF_SHADER_OUTPUT_DIR}/${shader}.spv)
        # raymarch.comp carries a host-substituted macro for custom
        # formula bodies; define it empty for the ahead-of-time build so
        # the shipped SPIR-V covers the built-ins.
        add_custom_command(
            OUTPUT ${dst}
            COMMAND ${GLSLC_EXECUTABLE}
                    -DOMF_CUSTOM_FORMULA_BODIES=
                    --target-env=vulkan1.3 -O -o ${dst} ${src}
            DEPENDS ${src}
            COMMENT "Compiling ${shader} -> SPIR-V"
            VERBATIM)
        list(APPEND OMF_SPIRV_FILES ${dst})
    endforeach()
    add_custom_target(omf_shaders ALL DEPENDS ${OMF_SPIRV_FILES})
elseif(OMF_WITH_VULKAN)
    message(WARNING "glslc not found -- shipping without precompiled SPIR-V; "
                    "the engine will use the CPU path at runtime")
endif()

# ---------------------------------------------------------------------
# OmniFractalCore
# ---------------------------------------------------------------------

set(OMF_CORE_SOURCES
    core/src/Renderer.cpp
    core/src/FormulaLibrary.cpp
    core/src/PrecisionProfiles.cpp
    core/src/CpuRaymarcher.cpp
    core/src/VulkanContext.cpp
    core/src/MarchingCubes.cpp
    core/src/StepExporter.cpp
    core/src/ExrExporter.cpp
    core/src/LuaEngine.cpp
    core/src/M4DFormat.cpp
    core/src/formats/LegacyImport.cpp)

add_library(OmniFractalCore SHARED ${OMF_CORE_SOURCES})

add_library(omf::core ALIAS OmniFractalCore)

target_include_directories(OmniFractalCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${OMF_PRIVATE_INCLUDES})

target_compile_definitions(OmniFractalCore
    PRIVATE
        OMF_CORE_BUILD
        OMF_INSTALL_SHADER_DIR="${CMAKE_INSTALL_PREFIX}/share/omf/shaders"
        ${OMF_PRIVATE_DEFS})

target_link_libraries(OmniFractalCore PRIVATE ${OMF_PRIVATE_LIBS})

# The .NET side loads "OmniFractalCore" by name; keep it identical on all
# platforms so [LibraryImport] needs no per-OS fixups.
set_target_properties(OmniFractalCore PROPERTIES
    OUTPUT_NAME OmniFractalCore
    PREFIX ""
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------
# Third-party notices
# ---------------------------------------------------------------------
#
# Attribution for the MIT/BSD/Apache dependencies is a binding license
# obligation, so the notices file is regenerated on every build rather
# than maintained by hand and allowed to drift.

add_custom_target(omf_third_party_notices ALL
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_JSON=${CMAKE_CURRENT_SOURCE_DIR}/cmake/third-party.json
            -DOUTPUT_FILE=${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.md
            -DWITH_VULKAN=${OMF_WITH_VULKAN}
            -DWITH_LUA=${OMF_WITH_LUA}
            -DWITH_TINYEXR=${OMF_WITH_TINYEXR}
            -DWITH_OPENCASCADE=${OMF_WITH_OPENCASCADE}
            -DWITH_GRPC=${OMF_WITH_GRPC}
            -DWITH_MINIZ=${OMF_WITH_MINIZ}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateNotices.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.md
    COMMENT "Generating THIRD-PARTY-NOTICES.md"
    VERBATIM)

add_dependencies(OmniFractalCore omf_third_party_notices)

if(MSVC)
    target_compile_options(OmniFractalCore PRIVATE /W4 /permissive- /fp:fast /bigobj)
    # __cplusplus reports 199711L without this, which breaks feature
    # detection in sol2 and in our own headers.
    target_compile_options(OmniFractalCore PRIVATE /Zc:__cplusplus)
else()
    target_compile_options(OmniFractalCore PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-ffast-math>)
endif()

if(TARGET omf_shaders)
    add_dependencies(OmniFractalCore omf_shaders)
endif()

# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------

if(OMF_BUILD_CLI)
    add_executable(omf-cli core/tools/cli_main.cpp)
    target_link_libraries(omf-cli PRIVATE OmniFractalCore)
    target_include_directories(omf-cli PRIVATE core/include)
endif()

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

if(OMF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
endif()

# ---------------------------------------------------------------------
# Fuzzers
# ---------------------------------------------------------------------
#
# Every parser that touches untrusted input gets a harness. Requires
# clang; libFuzzer is not available under GCC or MSVC.

if(OMF_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "OMF_BUILD_FUZZERS requires clang (libFuzzer)")
    endif()

    foreach(_fuzz_target omf_fuzz_legacy omf_fuzz_m4d)
        add_executable(${_fuzz_target} core/tests/fuzz/fuzz_legacy_import.cpp
                                       ${OMF_CORE_SOURCES})
        target_include_directories(${_fuzz_target}
            PRIVATE core/include ${OMF_PRIVATE_INCLUDES})
        target_compile_definitions(${_fuzz_target}
            PRIVATE OMF_CORE_BUILD
                    OMF_INSTALL_SHADER_DIR=\"\"
                    ${OMF_PRIVATE_DEFS})
        target_link_libraries(${_fuzz_target} PRIVATE ${OMF_PRIVATE_LIBS})
        # ASan + UBSan alongside the fuzzer: a bounds bug that does not
        # crash outright is exactly what these parsers must not have.
        target_compile_options(${_fuzz_target} PRIVATE
            -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(${_fuzz_target} PRIVATE
            -fsanitize=fuzzer,address,undefined)
    endforeach()

    target_compile_definitions(omf_fuzz_m4d PRIVATE OMF_FUZZ_M4D)
endif()

# ---------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS OmniFractalCore
    EXPORT omfTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY core/include/omf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(OMF_SPIRV_FILES)
    install(FILES ${OMF_SPIRV_FILES} DESTINATION share/omf/shaders)
endif()

install(DIRECTORY scripts/lua/ DESTINATION share/omf/scripts)

# Both files are required in any distribution: the EULA, and the
# third-party attributions it incorporates by reference (LICENSE 9.1).
install(FILES LICENSE DESTINATION share/doc/omf)
install(FILES ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.md DESTINATION share/doc/omf)

# ---------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------

message(STATUS "")
message(STATUS "OmniFractal ${PROJECT_VERSION} configuration")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vulkan compute : ${OMF_WITH_VULKAN}")
message(STATUS "  LuaJIT         : ${OMF_WITH_LUA}")
message(STATUS "  OpenEXR        : ${OMF_WITH_TINYEXR}")
message(STATUS "  OpenCASCADE    : ${OMF_WITH_OPENCASCADE} (dynamic link only)")
message(STATUS "  DEFLATE/miniz  : ${OMF_WITH_MINIZ}")
message(STATUS "  fuzzers        : ${OMF_BUILD_FUZZERS}")
message(STATUS "  gRPC dispatch  : ${OMF_WITH_GRPC}")
message(STATUS "  tests          : ${OMF_BUILD_TESTS}")
message(STATUS "")
