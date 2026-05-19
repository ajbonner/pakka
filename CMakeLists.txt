# CMakeLists.txt — Windows / MSVC build entry point.
#
# This is additive: the Unix build stays on the hand-written Makefile.
# CMake exists so cl.exe (and any IDE / build generator that consumes
# CMake) can produce pakka.exe on Windows. The two builds share src/,
# include/, and src/win/ but nothing else.

cmake_minimum_required(VERSION 3.15)
project(pakka VERSION 1.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

string(TIMESTAMP PAKKA_BUILD_DATE "%b %d, %Y")

# The library is every src/*.c except cli.c; cli.c is the CLI. Only
# pakka_*-prefixed symbols leak out of the static archive (enforced by
# make symbol-audit).
set(PAKKA_LIB_SOURCES
    src/common.c
    src/compat.c
    src/filesystem.c
    src/pakfile.c
)

set(PAKKA_CLI_SOURCES
    src/cli.c
)

if(WIN32)
    # tronkko/dirent is header-only; cli_add_folder_r in cli.c uses it
    # for recursive directory walking on Windows. wingetopt has a .c
    # file used by cli.c only.
    list(APPEND PAKKA_CLI_SOURCES src/win/wingetopt/getopt.c)
endif()

add_library(pakka STATIC ${PAKKA_LIB_SOURCES})
add_executable(pakka_exe ${PAKKA_CLI_SOURCES})

# Preserve build/cmake/pakka.exe path for the Windows CI job; renaming the
# CMake target to pakka_exe lets the static library hold the canonical
# `pakka` target name (libpakka.a / pakka.lib).
set_target_properties(pakka_exe PROPERTIES OUTPUT_NAME pakka)

target_link_libraries(pakka_exe PRIVATE pakka)

# Public header on the library target propagates -Iinclude to consumers
# (here that's just pakka_exe; in future, external callers). src/ stays
# PRIVATE — internal headers (common.h, compat.h, ...) are not public ABI.
target_include_directories(pakka PUBLIC include PRIVATE src)
target_include_directories(pakka_exe PRIVATE src)

if(WIN32)
    # dirent / wingetopt are vendored under src/win/. Both ultimately
    # serve cli.c (cli_add_folder_r needs dirent.h; getopt() is
    # CLI-only), but the library exposes none of them.
    target_include_directories(pakka PRIVATE src/win/dirent)
    target_include_directories(pakka_exe PRIVATE
        src/win/wingetopt
        src/win/dirent
    )
endif()

# Compile definitions apply to both targets. _DEBUG=1 mirrors the
# Makefile's CPPFLAGS so the library compiles under identical
# preprocessor state on both build systems.
foreach(_t pakka pakka_exe)
    target_compile_definitions(${_t} PRIVATE
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
        target_compile_options(${_t} PRIVATE /W4 /WX)
        target_compile_definitions(${_t} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
        )
    else()
        target_compile_options(${_t} PRIVATE -Wall -pedantic)
        # realpath() is gated on _XOPEN_SOURCE under glibc/musl. Matches Makefile.
        target_compile_definitions(${_t} PRIVATE _XOPEN_SOURCE=700)
    endif()
endforeach()

# C-API test executable. Links the public include/ surface against the
# static library, no internals. Invoked from tests/c_api_test.bats.
add_executable(c_api_test tests/c_api_test.c)
target_link_libraries(c_api_test PRIVATE pakka)
if(MSVC)
    target_compile_options(c_api_test PRIVATE /W4 /WX)
    target_compile_definitions(c_api_test PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
    )
else()
    target_compile_options(c_api_test PRIVATE -Wall -pedantic)
endif()
