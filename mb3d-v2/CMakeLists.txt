# SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
# Copyright (c) 2026 KenTread. All Rights Reserved.
# Proprietary and confidential -- see LICENSE for terms.
#
# Top-level build for MB3D-V2.
#
# Every third-party dependency is optional and gated behind an
# MB3D_WITH_* switch. The core builds and renders with none of them
# enabled -- that configuration is what CI compiles on every push, so a
# missing SDK on a contributor's machine can never break the build.

cmake_minimum_required(VERSION 3.25)

project(mb3d
    VERSION 0.1.0
    DESCRIPTION "MB3D-V2 -- 64-bit GPU fractal renderer with Mandelbulb 3D format import"
    LANGUAGES CXX C)

# ---------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------

option(MB3D_WITH_VULKAN       "Vulkan 1.3 compute backend (volk + VMA)" ON)
option(MB3D_WITH_LUA          "LuaJIT scripting via sol2"               ON)
option(MB3D_WITH_TINYEXR      "Multi-layer OpenEXR export"              ON)
# OCCT is LGPL-2.1 + exception. It is DYNAMICALLY linked, always. See the
# enforcement block below and THIRD-PARTY-NOTICES.md.
option(MB3D_WITH_OPENCASCADE  "STEP (ISO 10303) export via OCCT"        OFF)
option(MB3D_WITH_MINIZ        "DEFLATE support for legacy import"       ON)
option(MB3D_BUILD_FUZZERS     "Build libFuzzer parser harnesses"        OFF)
option(MB3D_WITH_GRPC         "Distributed tile dispatcher"             OFF)
option(MB3D_BUILD_TESTS       "Build the core test suite"               ON)
option(MB3D_BUILD_CLI         "Build the headless mb3d-cli renderer"    ON)
option(MB3D_FETCH_DEPS        "Fetch missing dependencies with FetchContent" ON)

# ---------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Symbols are opt-in: the ABI surface is exactly what MB3D_API marks.
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
    message(FATAL_ERROR "mb3d requires a 64-bit toolchain (found ${CMAKE_SIZEOF_VOID_P}-byte pointers)")
endif()

# ---------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------

set(MB3D_PRIVATE_LIBS "")
set(MB3D_PRIVATE_DEFS "")
set(MB3D_PRIVATE_INCLUDES "")

# -- GLM: header-only, used by the tooling and shader-side maths --------
find_package(glm QUIET)
if(NOT glm_FOUND AND MB3D_FETCH_DEPS)
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(glm)
endif()
if(TARGET glm::glm)
    list(APPEND MB3D_PRIVATE_LIBS glm::glm)
elseif(TARGET glm)
    list(APPEND MB3D_PRIVATE_LIBS glm)
endif()

# -- Vulkan: volk loads entry points at runtime, VMA does allocation ----
if(MB3D_WITH_VULKAN)
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(WARNING "Vulkan SDK not found -- building CPU-only. "
                        "Set MB3D_WITH_VULKAN=OFF to silence this.")
        set(MB3D_WITH_VULKAN OFF)
    endif()
endif()

if(MB3D_WITH_VULKAN)
    if(MB3D_FETCH_DEPS)
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

    list(APPEND MB3D_PRIVATE_DEFS MB3D_WITH_VULKAN)
    # volk dlopens the loader itself; linking Vulkan::Headers only keeps
    # the binary loadable on machines with no Vulkan runtime at all.
    list(APPEND MB3D_PRIVATE_LIBS Vulkan::Headers)
    if(TARGET volk::volk_headers)
        list(APPEND MB3D_PRIVATE_LIBS volk::volk_headers)
    elseif(TARGET volk)
        list(APPEND MB3D_PRIVATE_LIBS volk)
    endif()
    if(TARGET GPUOpen::VulkanMemoryAllocator)
        list(APPEND MB3D_PRIVATE_LIBS GPUOpen::VulkanMemoryAllocator)
    endif()
endif()

# -- LuaJIT + sol2 ------------------------------------------------------
if(MB3D_WITH_LUA)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LUAJIT IMPORTED_TARGET luajit)
    endif()

    if(NOT LUAJIT_FOUND)
        message(WARNING "LuaJIT not found -- scripting disabled. "
                        "Install luajit-dev or set MB3D_WITH_LUA=OFF.")
        set(MB3D_WITH_LUA OFF)
    else()
        if(MB3D_FETCH_DEPS)
            FetchContent_Declare(sol2
                GIT_REPOSITORY https://github.com/ThePhD/sol2.git
                GIT_TAG        v3.3.0
                GIT_SHALLOW    TRUE)
            FetchContent_MakeAvailable(sol2)
        endif()
        list(APPEND MB3D_PRIVATE_DEFS MB3D_WITH_LUA)
        list(APPEND MB3D_PRIVATE_LIBS PkgConfig::LUAJIT)
        if(TARGET sol2::sol2)
            list(APPEND MB3D_PRIVATE_LIBS sol2::sol2)
        endif()
    endif()
endif()

# -- tinyexr ------------------------------------------------------------
if(MB3D_WITH_TINYEXR)
    if(MB3D_FETCH_DEPS)
        FetchContent_Declare(tinyexr
            GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
            GIT_TAG        v1.0.8
            GIT_SHALLOW    TRUE)
        FetchContent_Populate(tinyexr)
        list(APPEND MB3D_PRIVATE_INCLUDES ${tinyexr_SOURCE_DIR})
        list(APPEND MB3D_PRIVATE_DEFS MB3D_WITH_TINYEXR)
    else()
        find_path(TINYEXR_INCLUDE_DIR tinyexr.h)
        if(TINYEXR_INCLUDE_DIR)
            list(APPEND MB3D_PRIVATE_INCLUDES ${TINYEXR_INCLUDE_DIR})
            list(APPEND MB3D_PRIVATE_DEFS MB3D_WITH_TINYEXR)
        else()
            message(WARNING "tinyexr not found -- EXR export disabled")
            set(MB3D_WITH_TINYEXR OFF)
        endif()
    endif()
endif()

# -- miniz: DEFLATE for the compressed legacy containers ---------------
if(MB3D_WITH_MINIZ)
    if(MB3D_FETCH_DEPS)
        FetchContent_Declare(miniz
            GIT_REPOSITORY https://github.com/richgel999/miniz.git
            GIT_TAG        3.0.2
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(miniz)
        list(APPEND MB3D_PRIVATE_DEFS MB3D_WITH_MINIZ)
        if(TARGET miniz)
            list(APPEND MB3D_PRIVATE_LIBS miniz)
        endif()
    else()
        find_path(MINIZ_INCLUDE_DIR miniz.h)
        find_library(MINIZ_LIBRARY miniz)
        if(MINIZ_INCLUDE_DIR AND MINIZ_LIBRARY)
            list(APPEND MB3D_PRIVATE_INCLUDES ${MINIZ_INCLUDE_DIR})
            list(APPEND MB3D_PRIVATE_LIBS ${MINIZ_LIBRARY})
            list(APPEND MB3D_PRIVATE_DEFS MB3D_WITH_MINIZ)
        else()
            # Without DEFLATE the legacy containers cannot be opened at
            # all, so this is a hard failure rather than a quiet
            # degradation -- import is the product's reason to exist.
            message(FATAL_ERROR
                "miniz not found and MB3D_FETCH_DEPS=OFF. Legacy import requires DEFLATE. "
                "Install miniz, enable MB3D_FETCH_DEPS, or set MB3D_WITH_MINIZ=OFF to "
                "build without legacy import.")
        endif()
    endif()
endif()

# -- OpenCASCADE --------------------------------------------------------
# Off by default: OCCT is a large SDK and only the STEP path needs it.
if(MB3D_WITH_OPENCASCADE)
    find_package(OpenCASCADE QUIET)
    if(OpenCASCADE_FOUND)
        # LICENSING GATE, NOT A PREFERENCE.
        #
        # OCCT is LGPL-2.1 with an exception. LGPL section 6 permits
        # combining it with proprietary software only if the user retains
        # the ability to relink against a modified library. Static linking
        # removes that ability and would subject all of MB3D-V2 to the
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
                        "Install a shared OCCT build, or set MB3D_WITH_OPENCASCADE=OFF "
                        "to build without STEP export.")
                endif()
            endif()
        endforeach()

        list(APPEND MB3D_PRIVATE_DEFS MB3D_WITH_OPENCASCADE)
        list(APPEND MB3D_PRIVATE_INCLUDES ${OpenCASCADE_INCLUDE_DIR})
        list(APPEND MB3D_PRIVATE_LIBS
            TKernel TKMath TKBRep TKTopAlgo TKPrim TKShHealing
            TKSTEP TKSTEPBase TKSTEPAttr TKXSBase TKG2d TKG3d TKGeomBase)
    else()
        message(WARNING "OpenCASCADE not found -- STEP export disabled")
        set(MB3D_WITH_OPENCASCADE OFF)
    endif()
endif()

find_package(Threads REQUIRED)
list(APPEND MB3D_PRIVATE_LIBS Threads::Threads)

# ---------------------------------------------------------------------
# Shader compilation
# ---------------------------------------------------------------------

set(MB3D_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/shaders)

if(MB3D_WITH_VULKAN)
    find_program(GLSLC_EXECUTABLE NAMES glslc glslangValidator
                 HINTS $ENV{VULKAN_SDK}/bin)
endif()

set(MB3D_SPIRV_FILES "")
if(GLSLC_EXECUTABLE)
    file(MAKE_DIRECTORY ${MB3D_SHADER_OUTPUT_DIR})
    foreach(shader raymarch.comp marching_cubes.comp)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/core/shaders/${shader})
        set(dst ${MB3D_SHADER_OUTPUT_DIR}/${shader}.spv)
        # raymarch.comp carries a host-substituted macro for custom
        # formula bodies; define it empty for the ahead-of-time build so
        # the shipped SPIR-V covers the built-ins.
        add_custom_command(
            OUTPUT ${dst}
            COMMAND ${GLSLC_EXECUTABLE}
                    -DMB3D_CUSTOM_FORMULA_BODIES=
                    --target-env=vulkan1.3 -O -o ${dst} ${src}
            DEPENDS ${src}
            COMMENT "Compiling ${shader} -> SPIR-V"
            VERBATIM)
        list(APPEND MB3D_SPIRV_FILES ${dst})
    endforeach()
    add_custom_target(mb3d_shaders ALL DEPENDS ${MB3D_SPIRV_FILES})
elseif(MB3D_WITH_VULKAN)
    message(WARNING "glslc not found -- shipping without precompiled SPIR-V; "
                    "the engine will use the CPU path at runtime")
endif()

# ---------------------------------------------------------------------
# MB3D_Core
# ---------------------------------------------------------------------

set(MB3D_CORE_SOURCES
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

add_library(MB3D_Core SHARED ${MB3D_CORE_SOURCES})

add_library(mb3d::core ALIAS MB3D_Core)

target_include_directories(MB3D_Core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${MB3D_PRIVATE_INCLUDES})

target_compile_definitions(MB3D_Core
    PRIVATE
        MB3D_CORE_BUILD
        MB3D_INSTALL_SHADER_DIR="${CMAKE_INSTALL_PREFIX}/share/mb3d/shaders"
        ${MB3D_PRIVATE_DEFS})

target_link_libraries(MB3D_Core PRIVATE ${MB3D_PRIVATE_LIBS})

# The .NET side loads "MB3D_Core" by name; keep it identical on all
# platforms so [LibraryImport] needs no per-OS fixups.
set_target_properties(MB3D_Core PROPERTIES
    OUTPUT_NAME MB3D_Core
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

add_custom_target(mb3d_third_party_notices ALL
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_JSON=${CMAKE_CURRENT_SOURCE_DIR}/cmake/third-party.json
            -DOUTPUT_FILE=${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.md
            -DWITH_VULKAN=${MB3D_WITH_VULKAN}
            -DWITH_LUA=${MB3D_WITH_LUA}
            -DWITH_TINYEXR=${MB3D_WITH_TINYEXR}
            -DWITH_OPENCASCADE=${MB3D_WITH_OPENCASCADE}
            -DWITH_GRPC=${MB3D_WITH_GRPC}
            -DWITH_MINIZ=${MB3D_WITH_MINIZ}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateNotices.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.md
    COMMENT "Generating THIRD-PARTY-NOTICES.md"
    VERBATIM)

add_dependencies(MB3D_Core mb3d_third_party_notices)

if(MSVC)
    target_compile_options(MB3D_Core PRIVATE /W4 /permissive- /fp:fast /bigobj)
    # __cplusplus reports 199711L without this, which breaks feature
    # detection in sol2 and in our own headers.
    target_compile_options(MB3D_Core PRIVATE /Zc:__cplusplus)
else()
    target_compile_options(MB3D_Core PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-ffast-math>)
endif()

if(TARGET mb3d_shaders)
    add_dependencies(MB3D_Core mb3d_shaders)
endif()

# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------

if(MB3D_BUILD_CLI)
    add_executable(mb3d-cli core/tools/cli_main.cpp)
    target_link_libraries(mb3d-cli PRIVATE MB3D_Core)
    target_include_directories(mb3d-cli PRIVATE core/include)
endif()

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

if(MB3D_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
endif()

# ---------------------------------------------------------------------
# Fuzzers
# ---------------------------------------------------------------------
#
# Every parser that touches untrusted input gets a harness. Requires
# clang; libFuzzer is not available under GCC or MSVC.

if(MB3D_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MB3D_BUILD_FUZZERS requires clang (libFuzzer)")
    endif()

    foreach(_fuzz_target mb3d_fuzz_legacy mb3d_fuzz_m4d)
        add_executable(${_fuzz_target} core/tests/fuzz/fuzz_legacy_import.cpp
                                       ${MB3D_CORE_SOURCES})
        target_include_directories(${_fuzz_target}
            PRIVATE core/include ${MB3D_PRIVATE_INCLUDES})
        target_compile_definitions(${_fuzz_target}
            PRIVATE MB3D_CORE_BUILD
                    MB3D_INSTALL_SHADER_DIR=\"\"
                    ${MB3D_PRIVATE_DEFS})
        target_link_libraries(${_fuzz_target} PRIVATE ${MB3D_PRIVATE_LIBS})
        # ASan + UBSan alongside the fuzzer: a bounds bug that does not
        # crash outright is exactly what these parsers must not have.
        target_compile_options(${_fuzz_target} PRIVATE
            -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(${_fuzz_target} PRIVATE
            -fsanitize=fuzzer,address,undefined)
    endforeach()

    target_compile_definitions(mb3d_fuzz_m4d PRIVATE MB3D_FUZZ_M4D)
endif()

# ---------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS MB3D_Core
    EXPORT mb3dTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY core/include/mb3d DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(MB3D_SPIRV_FILES)
    install(FILES ${MB3D_SPIRV_FILES} DESTINATION share/mb3d/shaders)
endif()

install(DIRECTORY scripts/lua/ DESTINATION share/mb3d/scripts)

# Both files are required in any distribution: the EULA, and the
# third-party attributions it incorporates by reference (LICENSE 9.1).
install(FILES LICENSE DESTINATION share/doc/mb3d)
install(FILES ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.md DESTINATION share/doc/mb3d)

# ---------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------

message(STATUS "")
message(STATUS "mb3d ${PROJECT_VERSION} configuration")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vulkan compute : ${MB3D_WITH_VULKAN}")
message(STATUS "  LuaJIT         : ${MB3D_WITH_LUA}")
message(STATUS "  OpenEXR        : ${MB3D_WITH_TINYEXR}")
message(STATUS "  OpenCASCADE    : ${MB3D_WITH_OPENCASCADE} (dynamic link only)")
message(STATUS "  DEFLATE/miniz  : ${MB3D_WITH_MINIZ}")
message(STATUS "  fuzzers        : ${MB3D_BUILD_FUZZERS}")
message(STATUS "  gRPC dispatch  : ${MB3D_WITH_GRPC}")
message(STATUS "  tests          : ${MB3D_BUILD_TESTS}")
message(STATUS "")
