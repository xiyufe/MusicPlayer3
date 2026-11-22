cmake_minimum_required(VERSION 3.24)

project(MusicPlayer3
    VERSION 0.1.0
    DESCRIPTION "Open source music player for Windows and Android"
    HOMEPAGE_URL "https://github.com/xiyufe/MusicPlayer3"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put every binary in one place so running a build does not require hunting
# through per-target subdirectories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(MP3_BUILD_TESTS "Build the core unit tests" ON)
option(MP3_ENABLE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Compiler warnings
#
# Kept strict from day one: the core is the part that must stay clean, and it
# is much cheaper to keep warning-free than to get back there later.
# ---------------------------------------------------------------------------

if(MSVC)
    set(MP3_WARNING_FLAGS /W4 /permissive-)
    if(MP3_ENABLE_WARNINGS_AS_ERRORS)
        list(APPEND MP3_WARNING_FLAGS /WX)
    endif()
else()
    set(MP3_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                          -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
                          -Wnull-dereference -Wdouble-promotion)
    if(MP3_ENABLE_WARNINGS_AS_ERRORS)
        list(APPEND MP3_WARNING_FLAGS -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------

add_subdirectory(core)

if(MP3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
endif()
