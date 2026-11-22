# MusicPlayer3 core -- a pure C++20 static library with no Qt dependency.
#
# Everything platform specific (WASAPI, AAudio) and everything third party
# (FFmpeg, TagLib, SQLite) is optional at configure time: the library always
# builds, and the capabilities it reports at runtime shrink when a backend is
# absent.  That keeps the tree compilable on a bare toolchain.

add_library(mp3_core STATIC
    src/core.cpp
    src/audio_format.cpp
    src/decoder.cpp
    src/audio_output.cpp
)

add_library(MusicPlayer3::core ALIAS mp3_core)

target_include_directories(mp3_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(mp3_core PUBLIC cxx_std_20)
target_compile_options(mp3_core PRIVATE ${MP3_WARNING_FLAGS})

target_compile_definitions(mp3_core
    PRIVATE MP3_CORE_BUILD
    PUBLIC
        MP3_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        MP3_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        MP3_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# --- Platform libraries ----------------------------------------------------
#
# Only what the always-built sources need.  Real backends add their own
# requirements as they arrive.

if(WIN32)
    target_link_libraries(mp3_core PRIVATE ole32 uuid)
elseif(ANDROID)
    target_link_libraries(mp3_core PRIVATE log android)
elseif(APPLE)
    target_link_libraries(mp3_core PRIVATE "-framework CoreAudio")
endif()

find_package(Threads REQUIRED)
target_link_libraries(mp3_core PUBLIC Threads::Threads)

# --- Optional backends -----------------------------------------------------
#
# Each block sets MP3_HAVE_<X> so source files can switch between the real
# implementation and the "not compiled in" path.

# FFmpeg -- decoding.  Looked up via pkg-config, which is how MSYS2, vcpkg,
# conan and every Linux distribution expose it.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample)
    if(FFMPEG_FOUND)
        target_link_libraries(mp3_core PRIVATE PkgConfig::FFMPEG)
        target_compile_definitions(mp3_core PRIVATE MP3_HAVE_FFMPEG=1)
        message(STATUS "MusicPlayer3: FFmpeg backend enabled (${FFMPEG_VERSION})")
    endif()
endif()
if(NOT FFMPEG_FOUND)
    message(STATUS "MusicPlayer3: FFmpeg not found -- decoder reports NotSupported")
endif()

# TagLib -- metadata and cover art.
if(PkgConfig_FOUND)
    pkg_check_modules(TAGLIB QUIET IMPORTED_TARGET taglib)
    if(TAGLIB_FOUND)
        target_link_libraries(mp3_core PRIVATE PkgConfig::TAGLIB)
        target_compile_definitions(mp3_core PRIVATE MP3_HAVE_TAGLIB=1)
        message(STATUS "MusicPlayer3: TagLib enabled (${TAGLIB_VERSION})")
    endif()
endif()

# SQLite -- media library.
if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3 QUIET IMPORTED_TARGET sqlite3)
    if(SQLITE3_FOUND)
        target_link_libraries(mp3_core PRIVATE PkgConfig::SQLITE3)
        target_compile_definitions(mp3_core PRIVATE MP3_HAVE_SQLITE3=1)
        message(STATUS "MusicPlayer3: SQLite enabled (${SQLITE3_VERSION})")
    endif()
endif()

# --- Install ---------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS mp3_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/mp3 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
