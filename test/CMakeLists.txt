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
# CMAKE_BINARY_DIR (not the test/ subdir) so Windows CI's
# `test -x build/cmake/<name>.exe` checks resolve directly.
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

# c_api_test (NULL tolerance, round-trips, accessors, verify, memory
# APIs) and dk_codec_test (opcode boundaries / overlap / bounds /
# termination) are the public-API regression binaries. Both are
# registered as native CTest targets.
#
# c_api_test takes two positional args: <pak0.pak> <scratch_dir>. The
# fixture lives at ${CMAKE_SOURCE_DIR}/build/test/pak0.pak (downloaded
# via `make fixture` on Unix; fixtures.ps1 on Windows lands it at the
# same path). The scratch dir needs to be fresh every run — we wire
# a CTest fixture pair that rm -rf's + mkdir's the scratch dir before
# c_api_test runs.
pakka_add_test_exe(c_api_test c_api_test.c)

add_test(NAME _c_api_scratch_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/test_scratch/c_api)
add_test(NAME _c_api_scratch_mkdir
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/test_scratch/c_api)
set_tests_properties(_c_api_scratch_clean PROPERTIES FIXTURES_SETUP c_api_scratch)
set_tests_properties(_c_api_scratch_mkdir PROPERTIES FIXTURES_SETUP c_api_scratch
    DEPENDS _c_api_scratch_clean)

add_test(NAME c_api_test
    COMMAND c_api_test
            ${CMAKE_SOURCE_DIR}/build/test/pak0.pak
            ${CMAKE_BINARY_DIR}/test_scratch/c_api)
set_tests_properties(c_api_test PROPERTIES FIXTURES_REQUIRED c_api_scratch)

pakka_add_test_exe(dk_codec_test dk_codec_test.c)
target_include_directories(dk_codec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME dk_codec_test COMMAND dk_codec_test)

# proc_self_test: standalone, no args/env needed. Registered with CTest
# directly — exercises proc.{h,c} by spawning itself in child mode.
pakka_add_test_exe(proc_self_test proc_self_test.c)
add_test(NAME proc_self_test COMMAND proc_self_test)

# large_file_test: 32-bit fseek/ftell ceiling regression. Synthesizes a
# 2.6 GB sparse pak whose directory sits above LONG_MAX. Needs PAKKA env
# pointing at the pakka binary; the scratch dir is absolute so the
# sparse fixture lives in a known location regardless of CWD at test
# invocation time.
pakka_add_test_exe(large_file_test large_file_test.c)
add_test(NAME large_file_test COMMAND large_file_test)
set_tests_properties(large_file_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;LARGE_FILE_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/large_file")

# pk4_test: PK4 (Doom 3) container — covers list / extract / verify /
# create / delete-rebuild against fixtures built with pakka -c --compress
# (the resulting DEFLATE entries exercise the same reader path a real
# Doom 3 PK4 hits).
pakka_add_test_exe(pk4_test pk4_test.c)
add_test(NAME pk4_test COMMAND pk4_test)
set_tests_properties(pk4_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;PK4_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/pk4")

# sin_test: SiN (Ritual, 1998) pak format. Inline binary writer for the
# SPAK synthesis cases; the other cases drive pakka itself to build
# fixtures.
pakka_add_test_exe(sin_test sin_test.c)
add_test(NAME sin_test COMMAND sin_test)
set_tests_properties(sin_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;SIN_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/sin")

# wad_test: Doom WAD format. Inline WAD writer + header inspection
# helpers; IWAD / PWAD / lump-directory coverage.
pakka_add_test_exe(wad_test wad_test.c)
add_test(NAME wad_test COMMAND wad_test)
set_tests_properties(wad_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;WAD_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/wad")

# dk_test: Daikatana pak format. Inline DK binary writer (72-byte
# entries, literal-run opcode encoder) + directory probe + real-fixture
# cross-validation against test/fixtures/dk/user.pak (REPO_ROOT env
# points at the source tree).
pakka_add_test_exe(dk_test dk_test.c)
add_test(NAME dk_test COMMAND dk_test)
set_tests_properties(dk_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;DK_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/dk;REPO_ROOT=${CMAKE_SOURCE_DIR}")

# pakka_test: Quake PAK format. End-to-end CLI surface against pak0.pak
# plus per-policy synthetic-pak cases. POSIX-only cases (symlink
# rejection, read-only chmod) gate on #ifndef _WIN32 and SKIP on
# Windows. Needs PAK0 env pointing at the canonical fixture; the
# fixture lives under the source tree's build/test/ (downloaded via
# `make fixture` on Unix; fixtures.ps1 on Windows lands it at the same
# path).
pakka_add_test_exe(pakka_test pakka_test.c)
add_test(NAME pakka_test COMMAND pakka_test)
set_tests_properties(pakka_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;PAK0=${CMAKE_SOURCE_DIR}/build/test/pak0.pak;PAKKA_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/pakka")

# pk3_test: PK3 (ZIP) container — end-to-end reader/writer through
# the pakka CLI plus in-process c-api harness (max_decompressed cap,
# commit-refuses-changed source, open_entry_handle, rebuild rollback)
# and fault-injection tests (atomic add, rebuild rollback after
# fclose / rename). Inline ZIP writer covers the malformed-ZIP
# rejection cases. Links libpakka for the in-process API calls.
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

# unicode_paths_test: UTF-8 path handling. Exercises proc.c's
# CreateProcessW + UTF-8 → UTF-16 conversion on Windows so the Cyrillic
# argv bytes reach pakka's wmain correctly; on POSIX exercises the
# CP1251 substitution / sanitization paths.
pakka_add_test_exe(unicode_paths_test unicode_paths_test.c)
add_test(NAME unicode_paths_test COMMAND unicode_paths_test)
set_tests_properties(unicode_paths_test PROPERTIES
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;UNICODE_TEST_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/unicode")
