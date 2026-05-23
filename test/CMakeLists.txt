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

# Helper: declare a test executable + CTest entry with uniform warning
# flags. Optional ARGS pass through to add_test's COMMAND; if omitted
# the binary runs with no arguments. ENVIRONMENT may be passed as a
# semicolon-separated list of NAME=VALUE strings.
function(pakka_add_c_test name source)
    cmake_parse_arguments(PT "" "" "ARGS;ENVIRONMENT" ${ARGN})

    add_executable(${name} ${source})
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

    add_test(NAME ${name} COMMAND ${name} ${PT_ARGS})
    if(PT_ENVIRONMENT)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${PT_ENVIRONMENT}")
    endif()
endfunction()

# c_api_test: takes <pak0.pak> <scratch_dir>. Fixture must be present
# at run time (fetch via `make fixture` on Unix or fixtures.ps1 on
# Windows). The argv positions are set so `ctest` runs the same way
# the Makefile-driven path does.
pakka_add_c_test(c_api_test c_api_test.c
    ARGS ${CMAKE_BINARY_DIR}/test/pak0.pak
         ${CMAKE_BINARY_DIR}/test/scratch)

# dk_codec_test reaches private headers under src/.
pakka_add_c_test(dk_codec_test dk_codec_test.c)
target_include_directories(dk_codec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# proc_self_test exercises proc.{h,c} by spawning itself in child mode.
pakka_add_c_test(proc_self_test proc_self_test.c)

# large_file_test: 32-bit fseek/ftell ceiling regression. C peer of
# test/large_file.bats. Needs PAKKA env pointing at the pakka binary;
# the scratch dir is absolute so the 2.6 GB sparse fixture lives in
# a known location regardless of CWD at test invocation time.
pakka_add_c_test(large_file_test large_file_test.c
    ENVIRONMENT
        "PAKKA=$<TARGET_FILE:pakka_exe>;LARGE_FILE_SCRATCH=${CMAKE_BINARY_DIR}/test_scratch/large_file")
