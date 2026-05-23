# Shared test-support library: assertion macros, cross-platform process
# spawn (proc.h), filesystem and text helpers. Linked into every C test
# binary.
file(GLOB TEST_SUPPORT_SOURCES support/*.c)
add_library(test_support STATIC ${TEST_SUPPORT_SOURCES})
target_include_directories(test_support PUBLIC support)

if(MSVC)
    target_compile_options(test_support PRIVATE /W4 /WX)
    target_compile_definitions(test_support PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
    )
else()
    target_compile_options(test_support PRIVATE -Wall -pedantic)
endif()

# Build a test executable + apply uniform warning flags. Output goes to
# CMAKE_BINARY_DIR (not the test/ subdir) so Windows CI's existing
# `test -x build/cmake/<name>.exe` checks keep working through the
# migration window.
function(pakka_add_test_exe name source)
    add_executable(${name} ${source})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_libraries(${name} PRIVATE pakka test_support)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX)
        target_compile_definitions(${name} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
        )
    else()
        target_compile_options(${name} PRIVATE -Wall -pedantic)
    endif()
endfunction()

# c_api_test and dk_codec_test continue to be invoked via their .bats
# wrappers — the wrappers set up PAK0 / scratch dirs the binaries need.
# Build them here so Windows CI finds build/cmake/<name>.exe; don't
# register them with CTest yet (the fixture wiring lands with the
# fixtures.ps1 work in a later phase).
pakka_add_test_exe(c_api_test c_api_test.c)
pakka_add_test_exe(dk_codec_test dk_codec_test.c)
target_include_directories(dk_codec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# proc_self_test: standalone, no args/env needed. Registered with CTest
# directly — exercises proc.{h,c} by spawning itself in child mode.
pakka_add_test_exe(proc_self_test proc_self_test.c)
add_test(NAME proc_self_test COMMAND proc_self_test)

# large_file_test: 32-bit fseek/ftell ceiling regression. C peer of
# test/large_file.bats. Needs PAKKA env pointing at the pakka binary;
# the scratch dir is absolute so the 2.6 GB sparse fixture lives in a
# known location regardless of CWD at test invocation time.
pakka_add_test_exe(large_file_test large_file_test.c)
add_test(NAME large_file_test COMMAND large_file_test)
set_tests_properties(large_file_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;LARGE_FILE_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/large_file")

# pk4_test: PK4 (Doom 3) container — partial C peer of test/pk4.bats.
# Covers the 4 cases that build fixtures via pakka itself.
pakka_add_test_exe(pk4_test pk4_test.c)
add_test(NAME pk4_test COMMAND pk4_test)
set_tests_properties(pk4_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;PK4_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/pk4")

# sin_test: SiN (Ritual, 1998) pak format. C peer of test/sin.bats —
# all 10 cases ported, with an inline binary writer for the SPAK
# synthesis cases.
pakka_add_test_exe(sin_test sin_test.c)
add_test(NAME sin_test COMMAND sin_test)
set_tests_properties(sin_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;SIN_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/sin")

# wad_test: Doom WAD format. C peer of test/wad.bats — all 20 cases
# ported, with an inline WAD writer + header inspection helpers.
pakka_add_test_exe(wad_test wad_test.c)
add_test(NAME wad_test COMMAND wad_test)
set_tests_properties(wad_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;WAD_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/wad")

# dk_test: Daikatana pak format. C peer of test/dk.bats — all 18 cases
# ported, including the real-fixture cross-validation against
# test/fixtures/dk/user.pak (REPO_ROOT env points at the source tree).
pakka_add_test_exe(dk_test dk_test.c)
add_test(NAME dk_test COMMAND dk_test)
set_tests_properties(dk_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;DK_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/dk;REPO_ROOT=${CMAKE_SOURCE_DIR}")

# pakka_test: Quake PAK format. Partial C peer of test/pakka.bats — the
# 46 cases that aren't POSIX-symlink / chmod / shell-pipeline specific.
# Needs PAK0 env pointing at the canonical fixture; the fixture lives
# under the source tree's build/test/ (downloaded via `make fixture` on
# Unix; fixtures.ps1 on Windows will land it at the same path).
pakka_add_test_exe(pakka_test pakka_test.c)
add_test(NAME pakka_test COMMAND pakka_test)
set_tests_properties(pakka_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;PAK0=${CMAKE_SOURCE_DIR}/build/test/pak0.pak;PAKKA_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/pakka")

# pk3_test: PK3 (ZIP) container — partial C peer of test/pk3.bats.
# Inline minimal ZIP writer (CRC32 table + LFH/CDR/EOCD with
# parametric fields) covers the malformed-ZIP rejection cases without
# needing /usr/bin/zip or python.
pakka_add_test_exe(pk3_test pk3_test.c)
add_test(NAME pk3_test COMMAND pk3_test)
set_tests_properties(pk3_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;PK3_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/pk3")

# pk3_q3demo_test: realpak suite against the Q3 demo pak0.pk3. Gated
# on Q3DEMO_PAK0_PK3 — fixture is downloaded via the realpak-test-q3
# Makefile target / fixtures.ps1 on Windows. Cases SKIP when unset.
pakka_add_test_exe(pk3_q3demo_test pk3_q3demo_test.c)
add_test(NAME pk3_q3demo_test COMMAND pk3_q3demo_test)
set_tests_properties(pk3_q3demo_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;Q3DEMO_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/q3demo")

# pak_goldsrc_test: realpak suite against Half-Life Uplink + Day One
# PAKs. Each fixture is independently env-var gated.
pakka_add_test_exe(pak_goldsrc_test pak_goldsrc_test.c)
add_test(NAME pak_goldsrc_test COMMAND pak_goldsrc_test)
set_tests_properties(pak_goldsrc_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;GOLDSRC_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/goldsrc")
