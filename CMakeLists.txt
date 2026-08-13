# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0

cmake_minimum_required(VERSION 3.16)

project(
    liboot
    VERSION 0.8.0
    DESCRIPTION "Embed Ocarina of Time's Link gameplay in a host engine"
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/decomp/src"
)

# Keep these definitions in sync with the native Makefile.  They select the
# ROM layout and compatibility paths expected by the vendored decompilation.
target_compile_definitions(oot PRIVATE
    LIBOOT_HOST_BUILD=1
    _LANGUAGE_C
    NON_MATCHING
    AVOID_UB
    DEBUG_FEATURES=0
    OOT_VERSION=PAL_1_1
    OOT_REVISION=1
    PLATFORM_N64=1
    PLATFORM_GC=0
    PLATFORM_IQUE=0
    F3DEX_GBI_2
    # Keep decompilation-era compatibility functions private to liboot. Static
    # hosts such as SM64 ports commonly provide functions with the original
    # libultra names, so leaving those names global makes otherwise valid
    # integrations fail at the final link step.
    osCreateMesgQueue=liboot_internal_osCreateMesgQueue
    osSendMesg=liboot_internal_osSendMesg
    osRecvMesg=liboot_internal_osRecvMesg
    guPerspective=liboot_internal_guPerspective
    guMtxF2L=liboot_internal_guMtxF2L
    guMtxIdent=liboot_internal_guMtxIdent
    guMtxIdentF=liboot_internal_guMtxIdentF
    sins=liboot_internal_sins
    coss=liboot_internal_coss
)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(oot PRIVATE LIBOOT_MULTI_INSTANCE=1)
endif()

# Shared-library sources export their declarations. Static consumers must not
# see dllimport on Windows; publish OOT_LIB_STATIC through both CMake and the
# generated pkg-config metadata.
if(BUILD_SHARED_LIBS)
    target_compile_definitions(oot PRIVATE OOT_LIB_EXPORT)
    set(LIBOOT_PC_CFLAGS "")
else()
    target_compile_definitions(oot PUBLIC OOT_LIB_STATIC=1)
    set(LIBOOT_PC_CFLAGS "-DOOT_LIB_STATIC=1")
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

# UCRT64's shared libgcc/libwinpthread DLLs are not part of a stock Windows
# installation. Link the compiler runtime into the DLL so release archives are
# self-contained without bundling toolchain runtime files and licenses.
if(MINGW AND BUILD_SHARED_LIBS)
    target_link_options(oot PRIVATE -static)
endif()

# ELF multi-instance discovery excludes GNU_RELRO. Bind imports eagerly so a
# lazy PLT slot cannot mutate outside the saved writable image after context
# capture. Other platforms do not use the ELF program-header path.
if(BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
    target_link_options(oot PRIVATE -Wl,-z,now)
endif()

if(BUILD_TESTING)
    function(liboot_configure_test target)
        set_target_properties(${target} PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED YES
            C_EXTENSIONS NO
        )
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4)
        else()
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic
            )
        endif()
        if(LIBOOT_ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE
                ${liboot_sanitizer_flags}
            )
            target_link_options(${target} PRIVATE
                ${liboot_sanitizer_flags}
            )
        endif()
    endfunction()

    add_executable(liboot_fidelity_runner test/fidelity_runner.c)
    liboot_configure_test(liboot_fidelity_runner)
    target_link_libraries(liboot_fidelity_runner PRIVATE liboot::oot)
    if(NOT WIN32)
        target_link_libraries(liboot_fidelity_runner PRIVATE m)
    endif()
    add_test(
        NAME fidelity-runner-self-test
        COMMAND liboot_fidelity_runner --self-test
    )

    add_executable(liboot_engine_init_test test/engine_init_test.c)
    liboot_configure_test(liboot_engine_init_test)
    target_link_libraries(liboot_engine_init_test PRIVATE liboot::oot)
    add_test(
        NAME engine-init-contract
        COMMAND liboot_engine_init_test
    )

    if(NOT BUILD_SHARED_LIBS)
        # Static game ports often carry their own libultra compatibility
        # layer. This executable deliberately owns names that have collided
        # in real hosts and must still link the complete engine.
        add_executable(liboot_static_namespace_test
            test/static_namespace_test.c
        )
        liboot_configure_test(liboot_static_namespace_test)
        target_link_libraries(liboot_static_namespace_test PRIVATE liboot::oot)
        add_test(
            NAME static-host-symbol-namespace
            COMMAND liboot_static_namespace_test
        )
    endif()

    # rom_util.c is deliberately tested directly because it is an internal,
    # hidden-symbol parser rather than part of the installed ABI.
    add_executable(liboot_rom_util_test
        test/rom_util_test.c
        src/rom_util.c
    )
    liboot_configure_test(liboot_rom_util_test)
    target_include_directories(liboot_rom_util_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME rom-util-synthetic-inputs
        COMMAND liboot_rom_util_test
    )

    # Header selection is an internal big-endian parser, so exercise it
    # directly with synthetic scene and room command streams.
    add_executable(liboot_scene_header_test
        test/scene_header_test.c
        src/scene_header.c
    )
    liboot_configure_test(liboot_scene_header_test)
    target_include_directories(liboot_scene_header_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME scene-header-synthetic-inputs
        COMMAND liboot_scene_header_test
    )

    add_executable(liboot_scene_room_image_test
        test/scene_room_image_test.c
        src/scene_room_image.c
    )
    liboot_configure_test(liboot_scene_room_image_test)
    target_include_directories(liboot_scene_room_image_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME scene-room-image-synthetic-inputs
        COMMAND liboot_scene_room_image_test
    )

    add_executable(liboot_audio_overflow_test test/audio_overflow_test.c)
    liboot_configure_test(liboot_audio_overflow_test)
    add_test(
        NAME audio-overflow-arithmetic
        COMMAND liboot_audio_overflow_test
    )

    # The fixed mixer is ROM-independent and has frozen command vectors. Build
    # the implementation directly so the internal arithmetic is testable even
    # though it is intentionally not part of the installed ABI.
    add_executable(liboot_audio_mixer_test
        test/audio_mixer_test.c
        src/audio_mixer.c
    )
    liboot_configure_test(liboot_audio_mixer_test)
    target_include_directories(liboot_audio_mixer_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME audio-fixed-mixer-vectors
        COMMAND liboot_audio_mixer_test
    )

    add_executable(liboot_enemy_bgm_test test/enemy_bgm_test.c)
    liboot_configure_test(liboot_enemy_bgm_test)
    target_link_libraries(liboot_enemy_bgm_test PRIVATE liboot::oot)
    add_test(
        NAME enemy-bgm-contract
        COMMAND liboot_enemy_bgm_test
    )

    add_executable(liboot_engine_limits_test test/engine_limits_test.c)
    liboot_configure_test(liboot_engine_limits_test)
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(liboot_engine_limits_test PRIVATE
            LIBOOT_EXPECT_MULTI_INSTANCE=1
        )
    endif()
    target_link_libraries(liboot_engine_limits_test PRIVATE liboot::oot)
    add_test(
        NAME engine-limits-contract
        COMMAND liboot_engine_limits_test
    )

    add_executable(liboot_host_actor_test test/host_actor_test.c)
    liboot_configure_test(liboot_host_actor_test)
    target_link_libraries(liboot_host_actor_test PRIVATE liboot::oot)
    add_test(
        NAME host-actor-contract
        COMMAND liboot_host_actor_test
    )

    # This test needs a user-supplied ROM at runtime, so it is compiled as a
    # contract probe but is intentionally not registered with CTest.
    add_executable(liboot_dynamic_collision_test test/dynamic_collision_test.c)
    liboot_configure_test(liboot_dynamic_collision_test)
    target_link_libraries(liboot_dynamic_collision_test PRIVATE liboot::oot)
    if(NOT WIN32)
        target_link_libraries(liboot_dynamic_collision_test PRIVATE m)
    endif()

    add_executable(liboot_scene_runtime_test test/scene_runtime_test.c)
    liboot_configure_test(liboot_scene_runtime_test)
    target_link_libraries(liboot_scene_runtime_test PRIVATE liboot::oot)
    if(NOT WIN32)
        target_link_libraries(liboot_scene_runtime_test PRIVATE m)
    endif()

    add_executable(liboot_scene_inventory_test test/scene_inventory_test.c)
    liboot_configure_test(liboot_scene_inventory_test)
    target_link_libraries(liboot_scene_inventory_test PRIVATE liboot::oot)

    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME rom-identity-synthetic-inputs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/rom_identity_test.py"
    )
    set_tests_properties(rom-identity-synthetic-inputs PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )

    # Hidden decomp symbols are intentionally absent from the shared ABI. A
    # static test can still exercise the exact collision-mask path that used
    # to perform 0x80 << 24 as a signed shift.
    if(NOT BUILD_SHARED_LIBS)
        add_executable(liboot_collision_mask_test test/collision_mask_test.c)
        liboot_configure_test(liboot_collision_mask_test)
        target_compile_definitions(liboot_collision_mask_test PRIVATE
            LIBOOT_HOST_BUILD=1
            _LANGUAGE_C
        )
        target_include_directories(liboot_collision_mask_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/decomp/include"
        )
        target_link_libraries(liboot_collision_mask_test PRIVATE liboot::oot)
        if(NOT WIN32)
            target_link_libraries(liboot_collision_mask_test PRIVATE m)
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
        "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/CONTRIBUTING.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/CONTRIBUTORS.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/SECURITY.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot"
)

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/LibUltraShip-MIT.txt"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/LICENSES"
)

# Preserve the repository-relative links used by README.md and the guides in
# installed documentation. Headers/binding sources are duplicated here only as
# readable references; their consumable copies remain under include/liboot.
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/GETTING_STARTED.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/USAGE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/API_REFERENCE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/UNIVERSAL_SDK.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/ENGINE_INTEGRATION.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/FIDELITY.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/ROM_COMPATIBILITY.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/DEVELOPMENT.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/RELEASING.md"
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
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/fuzz"
)
install(
    PROGRAMS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-install.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/identify-rom.py"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/tools"
)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/tools/rom-profiles.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liboot/tools"
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

# Derive the path back to the install prefix from the pkg-config directory.
# A fixed ../.. only works when CMAKE_INSTALL_LIBDIR is one component (lib or
# lib64), not for common multiarch layouts such as lib/x86_64-linux-gnu.
file(RELATIVE_PATH LIBOOT_PC_PREFIX_RELATIVE
    "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig"
    "${CMAKE_INSTALL_PREFIX}"
)
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(LIBOOT_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}/liboot")
else()
    set(LIBOOT_PC_INCLUDEDIR
        "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}/liboot"
    )
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/liboot.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/liboot.pc"
    @ONLY
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/liboot.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
