# Require CMake 3.25+ for C++23 (`cxx_std_23`), `FILE_SET HEADERS`, and
# `PROJECT_IS_TOP_LEVEL`.
cmake_minimum_required(VERSION 3.25)

# Declare the project name, semantic version, and that this is a C++-only repo
# (skips checking for a C compiler to speed up configure).
project(
  vsdmars_lib
  VERSION 0.1.0
  DESCRIPTION "Modern C++23 high-performance utility and container library"
  LANGUAGES CXX
)

# Emit `build/compile_commands.json` when building this repo directly so tools
# like clangd (Neovim LSP) and clang-tidy know the exact compiler flags.
# Skipped when another project includes vsdmars_lib as a subproject.
if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Toggle tests, install rules, and optional build-time clang-tidy checks.
# Tests/install default to ON when building vsdmars_lib standalone, and OFF when
# consumed via add_subdirectory() or FetchContent so we don't clutter a parent
# project's build/install targets.
option(VSDMARS_BUILD_TESTS "Build vsdmars_lib unit tests" ${PROJECT_IS_TOP_LEVEL})
option(VSDMARS_INSTALL "Generate install targets for vsdmars_lib" ${PROJECT_IS_TOP_LEVEL})
option(VSDMARS_ENABLE_CLANG_TIDY "Run clang-tidy (including .h headers) during build" OFF)

# When enabled, build the clang-tidy command list to attach per-target (rather
# than globally, so third-party FetchContent deps like GoogleTest aren't linted).
# `--header-filter` ensures all headers under `vsdmars/` are checked. On macOS,
# forward CMAKE_OSX_SYSROOT so Homebrew clang-tidy finds Apple SDK C++ headers.
if(VSDMARS_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(VSDMARS_CLANG_TIDY_CMD
    ${CLANG_TIDY_EXE}
    "--config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy"
    "--header-filter=.*vsdmars/.*"
  )
  if(APPLE AND CMAKE_OSX_SYSROOT)
    list(APPEND VSDMARS_CLANG_TIDY_CMD
      "--extra-arg=-isysroot"
      "--extra-arg=${CMAKE_OSX_SYSROOT}"
    )
  endif()
endif()

# ------------------------------------------------------------------------------
# Core library target
# ------------------------------------------------------------------------------
# INTERFACE means header-only (no .a/.so binary is built yet). If you add .cpp
# files later, change INTERFACE to STATIC or SHARED and add the .cpp files.
add_library(vsdmars_lib INTERFACE)

# Namespaced ALIAS so consumers always write `vsdmars::vsdmars_lib` whether
# they consume this repo via add_subdirectory/FetchContent OR find_package().
add_library(vsdmars::vsdmars_lib ALIAS vsdmars_lib)

# Require at least C++23 for any target that links against vsdmars::vsdmars_lib
# (CMake automatically passes `-std=c++23` or `/std:c++23` to the compiler).
target_compile_features(vsdmars_lib INTERFACE cxx_std_23)

# Register public headers using modern CMake FILE_SET HEADERS.
# - BASE_DIRS sets the root include path so users `#include <vsdmars/...>`.
# - Automatically populates both build-tree and install-tree include paths.
# - Add new public headers to FILES below as the library grows.
target_sources(
  vsdmars_lib
  INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES
      vsdmars/container/b_heap.h
)

# ------------------------------------------------------------------------------
# Install & CMake package config (enables `find_package(vsdmars_lib CONFIG)`)
# ------------------------------------------------------------------------------
if(VSDMARS_INSTALL)
  # Provides standard install paths like CMAKE_INSTALL_INCLUDEDIR (`include`)
  # and CMAKE_INSTALL_LIBDIR (`lib` or `lib64`).
  include(GNUInstallDirs)
  # Helpers to generate the *Config.cmake and *ConfigVersion.cmake files.
  include(CMakePackageConfigHelpers)

  # Record `vsdmars_lib` in the export set `vsdmars_libTargets` and copy all
  # headers in FILE_SET HEADERS into `<prefix>/include/vsdmars/...`.
  install(
    TARGETS vsdmars_lib
    EXPORT vsdmars_libTargets
    FILE_SET HEADERS
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # Generate and install `vsdmars_libTargets.cmake`, which defines the imported
  # target `vsdmars::vsdmars_lib` (include dirs + cxx_std_23) for consumers.
  install(
    EXPORT vsdmars_libTargets
    FILE vsdmars_libTargets.cmake
    NAMESPACE vsdmars::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vsdmars_lib
  )

  # Expand `cmake/vsdmars_libConfig.cmake.in` into the build dir so
  # `find_package(vsdmars_lib)` can locate and load `vsdmars_libTargets.cmake`.
  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vsdmars_libConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/vsdmars_libConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vsdmars_lib
  )

  # Generate version metadata so `find_package(vsdmars_lib 0.1 REQUIRED)` works.
  # ARCH_INDEPENDENT marks this package as usable on any CPU architecture
  # (remove ARCH_INDEPENDENT if you later compile architecture-specific .cpp files).
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/vsdmars_libConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
  )

  # Install both generated package config files under `<prefix>/lib/cmake/vsdmars_lib/`.
  install(
    FILES
      ${CMAKE_CURRENT_BINARY_DIR}/vsdmars_libConfig.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/vsdmars_libConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vsdmars_lib
  )
endif()

# ------------------------------------------------------------------------------
# Unit tests (GoogleTest + CTest)
# ------------------------------------------------------------------------------
if(VSDMARS_BUILD_TESTS)
  # Enables CTest and defines the standard CMake option `BUILD_TESTING` (default ON).
  include(CTest)
  if(BUILD_TESTING)
    # Use a system-installed GoogleTest if available; otherwise fetch it
    # automatically with FetchContent (and keep GTest out of install targets).
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
      include(FetchContent)
      set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
      set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
      FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
      )
      FetchContent_MakeAvailable(googletest)
    endif()

    # Provides `gtest_discover_tests()` to register each TEST() case with CTest.
    include(GoogleTest)

    # Build the BHeap test binary linked against our library (`-std=c++23` and
    # `<vsdmars/...>` include path) and GoogleTest's default `main()`.
    add_executable(b_heap_test vsdmars/container/test/b_heap_test.cpp)
    target_link_libraries(b_heap_test PRIVATE vsdmars::vsdmars_lib GTest::gtest_main)

    # Enable strict compiler warnings on tests without forcing them on consumers.
    if(MSVC)
      target_compile_options(b_heap_test PRIVATE /W4)
    else()
      target_compile_options(b_heap_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Attach clang-tidy only to our own target when requested.
    if(VSDMARS_ENABLE_CLANG_TIDY)
      set_target_properties(
        b_heap_test
        PROPERTIES CXX_CLANG_TIDY "${VSDMARS_CLANG_TIDY_CMD}"
      )
    endif()

    # Automatically discover and register every TEST() case in CTest.
    gtest_discover_tests(b_heap_test)
  endif()
endif()
