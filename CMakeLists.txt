cmake_minimum_required(VERSION 3.20)

project(FairnessGovernor
  VERSION 1.0.0
  DESCRIPTION "Generation-bound fairness governance runtime for competing network subjects"
  HOMEPAGE_URL "https://github.com/summonlabs/Fairness-Governor"
  LANGUAGES CXX)

# --- Global language settings -------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FG_BUILD_TOOLS "Build the Fairness Governor command line tools" ON)
option(FG_BUILD_TESTS "Build the Fairness Governor test suite" ON)
option(FG_BUILD_BENCH "Build the synthetic fairness benchmark" ON)
option(FG_BUILD_EXAMPLES "Build example programs" ON)
option(FG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(FG_ENABLE_ASAN "Enable AddressSanitizer (MSVC /fsanitize=address, GCC/Clang -fsanitize=address)" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# --- Strict warnings ----------------------------------------------------------
# Applied directly to every first-party target so that nothing the package exports
# depends on a build-only interface target.
function(fg_apply_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /Zc:__cplusplus
      /Zc:preprocessor
      /EHsc
      /MP
      /wd4820  # padding added after data member (noise from opaque structs)
      /wd4127  # constant conditional expression: the test harness deliberately
               # asserts relations between compile-time constants
    )
    if(FG_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE
      _CRT_SECURE_NO_WARNINGS
      WIN32_LEAN_AND_MEAN
      NOMINMAX
      _WIN32_WINNT=0x0A00
    )
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wcast-qual
      -Wold-style-cast
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
    )
    if(FG_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

if(FG_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
    add_compile_definitions(FG_ASAN_ENABLED=1)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
    add_compile_definitions(FG_ASAN_ENABLED=1)
  endif()
endif()

# --- Library ------------------------------------------------------------------
file(GLOB_RECURSE FG_LIBRARY_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(SORT FG_LIBRARY_SOURCES)

add_library(fairness_governor ${FG_LIBRARY_SOURCES})
add_library(FairnessGovernor::fairness_governor ALIAS fairness_governor)

target_include_directories(fairness_governor
  PUBLIC
    "\$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "\$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

fg_apply_strict_warnings(fairness_governor)

if(WIN32)
  target_link_libraries(fairness_governor PRIVATE ws2_32)
  # Winsock is used only through the private implementation; a consumer needs the
  # import library at link time on Windows.
  target_link_libraries(fairness_governor INTERFACE ws2_32)
endif()

set_target_properties(fairness_governor PROPERTIES
  OUTPUT_NAME fairness_governor
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

# --- Tools --------------------------------------------------------------------
if(FG_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# --- Examples -----------------------------------------------------------------
if(FG_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# --- Tests --------------------------------------------------------------------
if(FG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --- Benchmarks ---------------------------------------------------------------
if(FG_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# --- Installation -------------------------------------------------------------
install(TARGETS fairness_governor
  EXPORT FairnessGovernorTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT FairnessGovernorTargets
  FILE FairnessGovernorTargets.cmake
  NAMESPACE FairnessGovernor::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FairnessGovernor)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FairnessGovernorConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FairnessGovernorConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FairnessGovernor)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FairnessGovernorConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/FairnessGovernorConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FairnessGovernorConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FairnessGovernor)
