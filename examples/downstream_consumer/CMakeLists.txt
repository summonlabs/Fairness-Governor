# An independent downstream consumer of the installed FairnessGovernor package.
#
# This project is deliberately NOT part of the main build: it is configured on
# its own, against an installed tree, and only ever sees the exported CMake
# package and the installed headers.
cmake_minimum_required(VERSION 3.20)
project(FairnessGovernorConsumer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(FairnessGovernor 1.0 CONFIG REQUIRED)

if(NOT TARGET FairnessGovernor::fairness_governor)
  message(FATAL_ERROR "the exported target FairnessGovernor::fairness_governor is missing")
endif()

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE FairnessGovernor::fairness_governor)

if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME consumer_runs COMMAND consumer)
