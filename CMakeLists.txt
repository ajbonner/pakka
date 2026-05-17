# CMakeLists.txt — Windows / MSVC build entry point.
#
# This is additive: the Unix build stays on the hand-written Makefile.
# CMake exists so cl.exe (and any IDE / build generator that consumes
# CMake) can produce pakka.exe on Windows. The two builds share src/
# and src/win/ but nothing else.

cmake_minimum_required(VERSION 3.15)
project(pakka VERSION 1.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

string(TIMESTAMP PAKKA_BUILD_DATE "%b %d, %Y")

set(PAKKA_SOURCES
    src/common.c
    src/compat.c
    src/debug.c
    src/filesystem.c
    src/main.c
    src/pakfile.c
)

if(WIN32)
    # tronkko/dirent is header-only (dirent.h) so only wingetopt has a .c.
    list(APPEND PAKKA_SOURCES src/win/wingetopt/getopt.c)
endif()

add_executable(pakka ${PAKKA_SOURCES})

target_include_directories(pakka PRIVATE src)
if(WIN32)
    # wingetopt's getopt.c uses #include <getopt.h>, so its directory
    # must be on the angle-include search path. dirent likewise.
    target_include_directories(pakka PRIVATE
        src/win/wingetopt
        src/win/dirent
    )
endif()

target_compile_definitions(pakka PRIVATE
    "APP_NAME=\"Pakka\""
    "VERSION=\"${PROJECT_VERSION}\""
    "BUILD_DATE=\"${PAKKA_BUILD_DATE}\""
    _DEBUG=1
)

if(MSVC)
    # Warnings-as-errors mirrors the .clang-tidy WarningsAsErrors: '*'
    # policy on the Unix side. C4996 ("'fopen': This function may be
    # unsafe...") is silenced because pakka's input is constrained
    # and the _s variants would buy us nothing.
    target_compile_options(pakka PRIVATE /W4 /WX)
    target_compile_definitions(pakka PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
    )
else()
    target_compile_options(pakka PRIVATE -Wall -pedantic)
    # realpath() is gated on _XOPEN_SOURCE under glibc/musl. Matches Makefile.
    target_compile_definitions(pakka PRIVATE _XOPEN_SOURCE=700)
endif()
