# mh4n_cpu_interp: the fallback interpreter (N1-28, D13). The instruction handlers are generated at
# build time by tools/recomp/interp_gen.py from the recompiler's decoders; the generated C (about 800 KB)
# is not in the repository. Generation takes about a minute on two cores and is redone only when
# tools/recomp/*.py or recomp.h change.
#
# A copy of native/ on its own (tests/test_native_cmake_layout.py builds one) has no tools/ around it, and a
# machine without Python 3 cannot generate: the module is then empty, no boot step is registered, and a
# function that was not recompiled fails the program as before.
set(MH4N_REPO_ROOT "${MH4N_ROOT}/..")
set(MH4N_RECOMP_TOOLS "${MH4N_REPO_ROOT}/tools")
set(MH4N_RECOMP_RUNTIME "${MH4N_RECOMP_TOOLS}/recomp/runtime")
set(MH4N_FIBER_SOURCE "${MH4N_REPO_ROOT}/port/src/fiber.cpp")

find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_FOUND AND EXISTS "${MH4N_RECOMP_TOOLS}/recomp/interp_gen.py"
    AND EXISTS "${MH4N_RECOMP_RUNTIME}/recomp.h" AND EXISTS "${MH4N_FIBER_SOURCE}")
    set(mh4n_interp_generated "${CMAKE_CURRENT_BINARY_DIR}/ih_handlers.c")
    file(GLOB_RECURSE mh4n_interp_tool_sources CONFIGURE_DEPENDS
         "${MH4N_RECOMP_TOOLS}/recomp/*.py" "${MH4N_RECOMP_RUNTIME}/recomp.h")
    add_custom_command(
        OUTPUT "${mh4n_interp_generated}"
        COMMAND "${Python3_EXECUTABLE}" -m recomp.interp_gen --all --jobs 2 --out "${mh4n_interp_generated}"
        WORKING_DIRECTORY "${MH4N_RECOMP_TOOLS}"
        DEPENDS ${mh4n_interp_tool_sources}
        COMMENT "Generating the fallback interpreter's instruction handlers"
        VERBATIM)
    # The handlers are C compiled like the recompiled code (no floating-point contraction, rounding
    # honoured, flat guest memory), with the callbacks they make under names of this module's own.
    set_source_files_properties("${mh4n_interp_generated}" PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-frounding-math;-fno-strict-aliasing;-fwrapv"
        COMPILE_DEFINITIONS "RC_NATIVE;rc_svc=ih_host_svc;rc_trap=ih_host_trap")
    set(mh4n_interp_sources interp.cpp "${mh4n_interp_generated}")
    set(mh4n_interp_available ON)
else()
    message(STATUS "mh4n_cpu_interp: tools/recomp, port/src/fiber.cpp or Python 3 not found: an empty module")
    set(mh4n_interp_sources unavailable.cpp)
    set(mh4n_interp_available OFF)
endif()

mh4n_module(
    SOURCES ${mh4n_interp_sources}
    DEPENDS
        mh4n_memory
        mh4n_cpu
        mh4n_kernel
        mh4n_runtime
    THIRD_PARTY
        mh4n_fmt
)
# interp_test.cpp skips itself when the module is empty.
if (mh4n_interp_available)
    target_compile_definitions(mh4n_cpu_interp PUBLIC MH4N_INTERP_AVAILABLE=1)
endif()

# ---- test data (native/tests/interp_test.cpp) -------------------------------------------------------
# make_test_data.py builds the 3DS test program svc_test with three native code libraries (everything
# recompiled; without its worker functions; without any function) and runs random states through the C
# the recompiler emits for random words of every form, writing what the interpreter must reproduce. It
# needs Python 3 with Unicorn (requirements.txt, for the harness's machine model) and the clang tools that
# port/tests/build_svc_test.py calls; without them the cases of interp_test that use it skip (or fail with
# MH4N_REQUIRE_TEST_DATA=1). On Windows the libraries are built with llvm-mingw's x86_64-w64-mingw32-clang
# (as the code host's test libraries are) and there are no vectors: the harness that runs the recompiled
# side is for Linux and macOS.
if (mh4n_interp_available)
    find_program(MH4N_TOOL_CLANG NAMES clang)
    find_program(MH4N_TOOL_LLD NAMES ld.lld)
    find_program(MH4N_TOOL_OBJCOPY NAMES llvm-objcopy)
    find_program(MH4N_TOOL_READELF NAMES llvm-readelf)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import unicorn" RESULT_VARIABLE mh4n_have_unicorn
                    OUTPUT_QUIET ERROR_QUIET)
    set(MH4N_INTERP_DATA_DIR "${CMAKE_BINARY_DIR}/test-data/interp")
    set(MH4N_INTERP_PROGRAM "${MH4N_REPO_ROOT}/port/tests/svc_test.c")
    set(mh4n_interp_data_args "")
    set(mh4n_interp_data_outputs "${MH4N_INTERP_DATA_DIR}/svc_none.so")
    set(mh4n_interp_have_cc TRUE)
    if (WIN32)
        find_program(MH4N_TOOL_MINGW_CLANG NAMES x86_64-w64-mingw32-clang)
        if (MH4N_TOOL_MINGW_CLANG)
            list(APPEND mh4n_interp_data_args --cc "${MH4N_TOOL_MINGW_CLANG}" --no-vectors)
        else()
            set(mh4n_interp_have_cc FALSE)
        endif()
    else()
        list(APPEND mh4n_interp_data_outputs "${MH4N_INTERP_DATA_DIR}/vectors.bin")
    endif()
    if (mh4n_interp_have_cc AND MH4N_TOOL_CLANG AND MH4N_TOOL_LLD AND MH4N_TOOL_OBJCOPY AND MH4N_TOOL_READELF
        AND mh4n_have_unicorn EQUAL 0 AND EXISTS "${MH4N_INTERP_PROGRAM}")
        file(GLOB mh4n_interp_data_tools CONFIGURE_DEPENDS "${MH4N_REPO_ROOT}/tools/*.py")
        add_custom_command(
            OUTPUT ${mh4n_interp_data_outputs}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_test_data.py" "${MH4N_INTERP_DATA_DIR}"
                    ${mh4n_interp_data_args}
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_test_data.py" "${MH4N_INTERP_PROGRAM}"
                    "${MH4N_REPO_ROOT}/port/tests/svc.h" "${MH4N_REPO_ROOT}/port/tests/build_svc_test.py"
                    ${mh4n_interp_tool_sources} ${mh4n_interp_data_tools}
            COMMENT "Building the fallback interpreter's test data"
            VERBATIM)
        add_custom_target(mh4n_cpu_interp_test_data ALL DEPENDS ${mh4n_interp_data_outputs})
        target_compile_definitions(mh4n_cpu_interp PUBLIC
            "MH4N_INTERP_TEST_DATA_DIR=\"${MH4N_INTERP_DATA_DIR}\""
            "MH4N_INTERP_EXPECTED_DIR=\"${MH4N_REPO_ROOT}/port/tests/expected\"")
    else()
        message(STATUS "mh4n_cpu_interp: no test data (needs Python 3 with Unicorn, clang, ld.lld, "
                       "llvm-objcopy and llvm-readelf, and on Windows x86_64-w64-mingw32-clang): the cases of "
                       "interp_test that use it will skip")
    endif()
endif()
