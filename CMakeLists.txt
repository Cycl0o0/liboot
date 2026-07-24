# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0

cmake_minimum_required(VERSION 3.16)

project(
    liboot
    VERSION 0.8.0
    DESCRIPTION "Engine-neutral Ocarina of Time gameplay library"
    HOMEPAGE_URL "https://github.com/Cycl0o0/liboot"
    LANGUAGES C
)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

option(BUILD_SHARED_LIBS "Build liboot as a shared library" ON)
option(LIBOOT_ENABLE_SANITIZERS
    "Build liboot with the selected compiler sanitizers" OFF
)
set(LIBOOT_SANITIZERS "address,undefined" CACHE STRING
    "Comma-separated sanitizer list used when LIBOOT_ENABLE_SANITIZERS is ON"
)

file(GLOB_RECURSE LIBOOT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)
list(FILTER LIBOOT_SOURCES EXCLUDE REGEX "\\.inc\\.c$")

if(NOT LIBOOT_SOURCES)
    message(FATAL_ERROR "No liboot sources were found")
endif()

add_library(oot ${LIBOOT_SOURCES})
add_library(liboot::oot ALIAS oot)

set_target_properties(oot PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
    OUTPUT_NAME oot
    POSITION_INDEPENDENT_CODE YES
    C_VISIBILITY_PRESET hidden
    VERSION "${PROJECT_VERSION}"
    # Before 1.0, a minor release may intentionally revise the ABI.
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
)

# Keep the managed binding's native name stable on every platform. Unix adds
# its conventional `lib` prefix to `oot`; Windows needs the full basename.
if(WIN32 AND CMAKE_SHARED_LIBRARY_PREFIX STREQUAL "")
    set_target_properties(oot PROPERTIES OUTPUT_NAME liboot)
endif()

# A downloaded macOS dylib must be relocatable: without an @rpath install name
# CMake bakes the absolute build-machine path into the library and every
# consumer (including the C# DllImport resolver) fails to load it.
if(APPLE)
    set_target_properties(oot PROPERTIES
        MACOSX_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
endif()

target_include_directories(oot
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/liboot>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/decomp/include"
)

# Keep these definitions in sync with the native Makefile.  They select the
# ROM layout and compatibility paths expected by the vendored decompilation.
target_compile_definitions(oot PRIVATE
    _LANGUAGE_C
    NON_MATCHING
    AVOID_UB
    DEBUG_FEATURES=0
    OOT_VERSION=NTSC_1_2
    OOT_REVISION=2
    PLATFORM_N64=1
    PLATFORM_GC=0
    PLATFORM_IQUE=0
    F3DEX_GBI_2
)

# Library sources always need exported declarations.  A static Windows target
# also propagates the macro so its consumers do not treat those declarations
# as DLL imports; other consumers keep a clean compile interface.
target_compile_definitions(oot PRIVATE OOT_LIB_EXPORT)
if(NOT BUILD_SHARED_LIBS AND WIN32)
    target_compile_definitions(oot INTERFACE OOT_LIB_EXPORT)
endif()

if(MSVC)
    target_compile_options(oot PRIVATE /W4 /J)
else()
    target_compile_options(oot PRIVATE
        -Wall
        -Wno-unused-function
        -Wno-unused-variable
        -fno-strict-aliasing
        -funsigned-char
    )
endif()

if(LIBOOT_ENABLE_SANITIZERS)
    if(MSVC OR NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR
            "LIBOOT_ENABLE_SANITIZERS requires a GCC- or Clang-compatible compiler"
        )
    endif()
    if(LIBOOT_SANITIZERS STREQUAL "")
        message(FATAL_ERROR
            "LIBOOT_SANITIZERS must name at least one sanitizer"
        )
    endif()

    set(liboot_sanitizer_flags
        "-fsanitize=${LIBOOT_SANITIZERS}"
        -fno-omit-frame-pointer
    )
    target_compile_options(oot PRIVATE ${liboot_sanitizer_flags})
    target_link_options(oot PRIVATE ${liboot_sanitizer_flags})
    # A static archive has no final link step of its own, so its consumer must
    # receive the runtime flags. Shared builds link the runtime into liboot.
    if(NOT BUILD_SHARED_LIBS)
        target_link_options(oot INTERFACE ${liboot_sanitizer_flags})
    endif()
endif()

if(NOT WIN32)
    target_link_libraries(oot PRIVATE m)
    set(LIBOOT_PC_PRIVATE_LIBS "-lm")
else()
    set(LIBOOT_PC_PRIVATE_LIBS "")
endif()

if(BUILD_TESTING)
    add_executable(liboot_fidelity_runner test/fidelity_runner.c)
    set_target_properties(liboot_fidelity_runner PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(liboot_fidelity_runner PRIVATE liboot::oot)
    if(NOT WIN32)
        target_link_libraries(liboot_fidelity_runner PRIVATE m)
    endif()
    if(MSVC)
        target_compile_options(liboot_fidelity_runner PRIVATE /W4)
    else()
        target_compile_options(liboot_fidelity_runner PRIVATE
            -Wall -Wextra -Wpedantic
        )
    endif()
    if(LIBOOT_ENABLE_SANITIZERS)
        target_compile_options(liboot_fidelity_runner PRIVATE
            ${liboot_sanitizer_flags}
        )
        target_link_options(liboot_fidelity_runner PRIVATE
            ${liboot_sanitizer_flags}
        )
    endif()
    add_test(
        NAME fidelity-runner-self-test
        COMMAND liboot_fidelity_runner --self-test
    )

    # Hidden decomp symbols are intentionally absent from the shared ABI. A
    # static test can still exercise the exact collision-mask path that used
    # to perform 0x80 << 24 as a signed shift.
    if(NOT BUILD_SHARED_LIBS)
        add_executable(liboot_collision_mask_test test/collision_mask_test.c)
        set_target_properties(liboot_collision_mask_test PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED YES
            C_EXTENSIONS NO
        )
        target_compile_definitions(liboot_collision_mask_test PRIVATE
            _LANGUAGE_C
        )
        target_include_directories(liboot_collision_mask_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/decomp/include"
        )
        target_link_libraries(liboot_collision_mask_test PRIVATE liboot::oot)
        if(NOT WIN32)
            target_link_libraries(liboot_collision_mask_test PRIVATE m)
        endif()
        if(MSVC)
            target_compile_options(liboot_collision_mask_test PRIVATE /W4)
        else()
            target_compile_options(liboot_collision_mask_test PRIVATE
                -Wall -Wextra -Wpedantic
            )
        endif()
        if(LIBOOT_ENABLE_SANITIZERS)
            target_compile_options(liboot_collision_mask_test PRIVATE
                ${liboot_sanitizer_flags}
            )
            target_link_options(liboot_collision_mask_test PRIVATE
                ${liboot_sanitizer_flags}
            )
        endif()
        add_test(
            NAME collision-mask-signed-shift-regression
            COMMAND liboot_collision_mask_test
        )
    endif()
endif()

# Decompilation translation units intentionally use the game's libc headers;
# the host-facing sources must continue to use the platform libc instead.
foreach(source_file IN LISTS LIBOOT_SOURCES)
    if(source_file MATCHES "/src/decomp/")
        set_property(SOURCE "${source_file}" APPEND PROPERTY INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/src/decomp/include/libc"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/decomp"
        )
    endif()
endforeach()

install(
    TARGETS oot
    EXPORT libootTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/liboot.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/liboot_engine.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/liboot.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/liboot"
)

install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot"
)

# Preserve the repository-relative links used by README.md and the guides in
# installed documentation. Headers/binding sources are duplicated here only as
# readable references; their consumable copies remain under include/liboot.
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/GETTING_STARTED.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/UNIVERSAL_SDK.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/ENGINE_INTEGRATION.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/FIDELITY.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/docs"
)
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/liboot.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/liboot_engine.h"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/src"
)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/bindings/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/bindings"
)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/liboot.hpp"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/bindings/cpp"
)
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/bindings/csharp/LibOot.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/bindings/csharp/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/bindings/csharp"
)
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/engine.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/basic.c"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/examples"
)

set(liboot_cmake_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/liboot")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libootConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libootConfig.cmake"
    INSTALL_DESTINATION "${liboot_cmake_install_dir}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libootConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    # Before 1.0, a new minor release may change the ABI.
    COMPATIBILITY SameMinorVersion
)

install(
    EXPORT libootTargets
    FILE libootTargets.cmake
    NAMESPACE liboot::
    DESTINATION "${liboot_cmake_install_dir}"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libootConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libootConfigVersion.cmake"
    DESTINATION "${liboot_cmake_install_dir}"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/liboot.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/liboot.pc"
    @ONLY
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/liboot.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
