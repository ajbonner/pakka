# CMakeLists.txt — Windows / MSVC build entry point.
#
# This is additive: the Unix build stays on the hand-written Makefile.
# CMake exists so cl.exe (and any IDE / build generator that consumes
# CMake) can produce pakka.exe on Windows. The two builds share src/,
# include/, and src/vendor/ but nothing else.

cmake_minimum_required(VERSION 3.15)
project(pakka VERSION 1.7.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Detect standalone vs. add_subdirectory() consumption. The flags below
# default ON for standalone and OFF when pakka is a subdirectory, so the
# CLI binary, test targets, install rules, and CPack's `package` target
# don't leak into a parent project's build. Consumers can flip any flag
# back on explicitly.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PAKKA_IS_TOP_LEVEL ON)
else()
    set(PAKKA_IS_TOP_LEVEL OFF)
endif()

option(PAKKA_BUILD_CLI     "Build pakka CLI (pakka_exe)"   ${PAKKA_IS_TOP_LEVEL})
option(PAKKA_BUILD_TESTING "Build pakka test binaries"     ${PAKKA_IS_TOP_LEVEL})
option(PAKKA_INSTALL       "Generate pakka install rules"  ${PAKKA_IS_TOP_LEVEL})
option(PAKKA_PACKAGE       "Enable pakka CPack packaging"  ${PAKKA_IS_TOP_LEVEL})

string(TIMESTAMP PAKKA_BUILD_DATE "%b %d, %Y")

# DEFLATE backend selection. Default: bundled sdefl + sinfl. Opt-in:
# -DPAKKA_USE_ZLIB=ON routes through the host's zlib (raw DEFLATE via
# windowBits = -MAX_WBITS) instead, useful for integrators that
# already link libz and don't want to carry duplicate codec code.
# Exactly one backend impl is compiled per build.
option(PAKKA_USE_ZLIB "Use system zlib instead of bundled sdefl+sinfl" OFF)

# The library is every src/*.c except cli.c; cli.c is the CLI. Only
# pakka_*-prefixed symbols leak out of the static archive (enforced by
# make symbol-audit).
set(PAKKA_LIB_SOURCES
    src/common.c
    src/platform.c
    src/dk_codec.c
    src/filesystem.c
    src/pakfile.c
    src/pk3file.c
)

if(PAKKA_USE_ZLIB)
    list(APPEND PAKKA_LIB_SOURCES src/deflate/deflate_zlib.c)
else()
    list(APPEND PAKKA_LIB_SOURCES
        src/deflate/deflate_vendored.c
        src/vendor/sdefl/sdefl.c
        src/vendor/sinfl/sinfl.c
    )
endif()

set(PAKKA_CLI_SOURCES
    src/cli.c
)

if(WIN32)
    # tronkko/dirent is header-only; cli_add_folder_r in cli.c uses it
    # for recursive directory walking on Windows. wingetopt has a .c
    # file used by cli.c only.
    list(APPEND PAKKA_CLI_SOURCES src/vendor/wingetopt/getopt.c)
endif()

add_library(pakka STATIC ${PAKKA_LIB_SOURCES})
# Namespaced alias for downstream `target_link_libraries(... pakka::pakka)`
# whether pakka is vendored via add_subdirectory() or installed and
# pulled in via find_package().
add_library(pakka::pakka ALIAS pakka)

if(PAKKA_USE_ZLIB)
    find_package(ZLIB REQUIRED)
    target_link_libraries(pakka PUBLIC ZLIB::ZLIB)
endif()

# Vendored sources are not pakka's code — they ship with the warnings
# their upstream is willing to live with. Suppress /W4 and similar for
# them so MSVC's "warnings as errors" policy doesn't reject sdefl /
# sinfl (which use C4324 alignment padding, C4244 int->short etc.).
if(MSVC AND NOT PAKKA_USE_ZLIB)
    set_source_files_properties(src/vendor/sdefl/sdefl.c PROPERTIES
        COMPILE_FLAGS "/W0")
    set_source_files_properties(src/vendor/sinfl/sinfl.c PROPERTIES
        COMPILE_FLAGS "/W0")
endif()

# Public header on the library target propagates -Iinclude to consumers
# (here that's just pakka_exe; in future, external callers). src/ stays
# PRIVATE — internal headers (common.h, platform.h, ...) are not public ABI.
target_include_directories(pakka PUBLIC include PRIVATE src)

if(WIN32)
    # platform.c uses dirent for directory walking on Windows; keep it
    # PRIVATE so the header doesn't leak to consumers of libpakka.
    target_include_directories(pakka PRIVATE src/vendor/dirent)
endif()

if(PAKKA_BUILD_CLI)
    add_executable(pakka_exe ${PAKKA_CLI_SOURCES})

    # Preserve build/cmake/pakka.exe path for the Windows CI job; renaming
    # the CMake target to pakka_exe lets the static library hold the
    # canonical `pakka` target name (libpakka.a / pakka.lib).
    set_target_properties(pakka_exe PROPERTIES OUTPUT_NAME pakka)

    target_link_libraries(pakka_exe PRIVATE pakka)
    target_include_directories(pakka_exe PRIVATE src)

    if(WIN32)
        # wingetopt's getopt() and dirent are both CLI-only consumers
        # here. dirent appears again because pakka exposes it only
        # PRIVATE; pakka_exe needs it on its own include path.
        target_include_directories(pakka_exe PRIVATE
            src/vendor/wingetopt
            src/vendor/dirent
        )
    endif()
endif()

# Compile definitions apply to every pakka-owned target. _DEBUG=1
# mirrors the Makefile's CPPFLAGS so the library compiles under
# identical preprocessor state on both build systems. pakka_exe joins
# the list only when CLI builds are enabled.
set(_pakka_compile_targets pakka)
if(PAKKA_BUILD_CLI)
    list(APPEND _pakka_compile_targets pakka_exe)
endif()
foreach(_t IN LISTS _pakka_compile_targets)
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
        # _FILE_OFFSET_BITS=64 widens off_t on 32-bit glibc so pakka_platform_fseek
        # can address paks in [2 GiB, 4 GiB). No-op elsewhere.
        target_compile_definitions(${_t} PRIVATE _XOPEN_SOURCE=700 _FILE_OFFSET_BITS=64)
    endif()
endforeach()

# Test-build mode mirrors the Makefile's `make test` flag: defines
# PAKKA_TEST_BUILD so src/platform.c's fault-injection hooks compile in.
# Default OFF — release builds and the standalone `cmake --build` flow
# never touch the hooks. Windows CI sets it ON in the test workflow.
option(PAKKA_TEST_BUILD "Enable fault-injection hooks in libpakka" OFF)
if(PAKKA_TEST_BUILD)
    target_compile_definitions(pakka PRIVATE PAKKA_TEST_BUILD=1)
endif()

# Test binaries and their CTest registrations live under test/. The
# top-level build still produces them under the build dir (Windows CI
# relies on build/cmake/c_api_test.exe etc.). Gated so parent projects
# that vendor pakka don't get pakka's tests mixed into their CTest run.
if(PAKKA_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()

# Install layout mirrors the Unix Makefile install target:
#   bin/pakka(.exe)              — CLI binary
#   lib/pakka.lib (libpakka.a)   — static library
#   include/pakka.h              — public header
#   share/man/man1/pakka.1       — manpage
#   share/doc/pakka/{README,LICENSE} — docs
# GNUInstallDirs picks platform-appropriate destinations; CPack ZIP
# consumes the same install() rules to produce the Windows release zip.
# Both GNUInstallDirs (which adds CMAKE_INSTALL_* cache entries) and the
# install() rules sit behind PAKKA_INSTALL so a parent project vendoring
# pakka doesn't see pakka's bin/lib/include/share land in its install
# prefix.
if(PAKKA_INSTALL)
    include(GNUInstallDirs)

    if(PAKKA_BUILD_CLI)
        install(TARGETS pakka_exe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    install(TARGETS pakka     ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/pakka.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES man/pakka.1     DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
    install(FILES man/pakka.3     DESTINATION ${CMAKE_INSTALL_MANDIR}/man3)
    install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

# CPack ZIP generator — `cpack -G ZIP` (from the build dir) writes
# pakka-${PROJECT_VERSION}-${PAKKA_PACKAGE_PLATFORM}.zip with the
# layout above. Used by the Windows release workflow; safe to invoke on
# any host. The release workflow passes windows-x86 for 32-bit MSVC
# builds; the default preserves the original x64 package name.
#
# Whole block is gated so include(CPack) — which registers a global
# `package` target and writes CPackConfig.cmake at the top of the build
# tree — doesn't fire when pakka is a subdirectory. CMAKE_CURRENT_SOURCE_DIR
# (not CMAKE_SOURCE_DIR) anchors the LICENSE path to pakka's own root;
# under add_subdirectory(), CMAKE_SOURCE_DIR would point at the parent.
if(PAKKA_PACKAGE)
    set(PAKKA_PACKAGE_PLATFORM "windows-x64" CACHE STRING
        "Platform suffix for binary release packages")
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_NAME "pakka")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_FILE_NAME "pakka-${PROJECT_VERSION}-${PAKKA_PACKAGE_PLATFORM}")
    set(CPACK_PACKAGE_VENDOR "Aaron Bonner")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "CLI utility for game-engine archive formats (PAK, PK3, PK4, SiN, WAD)")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    include(CPack)
endif()
