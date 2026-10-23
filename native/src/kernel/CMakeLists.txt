# mh4n_kernel: kernel objects, the handle table and the Kernel facade (N1-05), the scheduler, the game
# clock and the timer queue (N1-06; the code host it drives is mh4n_cpu). N1-07 (SVCs) and N1-27
# (threads) add their .cpp files to this directory; the glob picks them up
# without editing this file (add `DEPENDS` for the modules they use when they need to).
file(GLOB kernel_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(SORT kernel_sources)

mh4n_module(
    SOURCES ${kernel_sources}
    DEPENDS mh4n_memory mh4n_cpu
)

# ---- test data for native/tests/determinism_test.cpp (N1-25) ----------------------------------------
# port/tests/svc_test.c (threads, mutexes, events, timers, an address arbiter) is built into a 3DS ELF
# and recompiled into a deterministic code library (rc_build_mode 2, the one --deterministic loads),
# under <build>/test-data/determinism. The test finds the folder through the compile definition below
# and skips the cases that need it when it is empty. Same tools and same Windows rule as the code host's
# test data (native/src/cpu/CMakeLists.txt).
set(MH4N_REPO_ROOT "${MH4N_ROOT}/..")
set(MH4N_DETERMINISM_DATA_DIR "${CMAKE_BINARY_DIR}/test-data/determinism")
set(MH4N_DETERMINISM_PROGRAM "${MH4N_REPO_ROOT}/port/tests/svc_test.c")
find_package(Python3 QUIET COMPONENTS Interpreter)
find_program(MH4N_DET_TOOL_CLANG NAMES clang)
find_program(MH4N_DET_TOOL_LLD NAMES ld.lld)
find_program(MH4N_DET_TOOL_OBJCOPY NAMES llvm-objcopy)
find_program(MH4N_DET_TOOL_READELF NAMES llvm-readelf)
set(determinism_cc_args "")
set(determinism_have_cc TRUE)
if (WIN32)
    find_program(MH4N_DET_TOOL_MINGW_CLANG NAMES x86_64-w64-mingw32-clang)
    if (MH4N_DET_TOOL_MINGW_CLANG)
        set(determinism_cc_args --cc "${MH4N_DET_TOOL_MINGW_CLANG}")
    else()
        set(determinism_have_cc FALSE)
    endif()
endif()
if (determinism_have_cc AND Python3_FOUND AND MH4N_DET_TOOL_CLANG AND MH4N_DET_TOOL_LLD
    AND MH4N_DET_TOOL_OBJCOPY AND MH4N_DET_TOOL_READELF AND EXISTS "${MH4N_DETERMINISM_PROGRAM}")
    file(GLOB_RECURSE determinism_tool_sources CONFIGURE_DEPENDS
         "${MH4N_REPO_ROOT}/tools/recomp/*.py" "${MH4N_REPO_ROOT}/tools/recomp/runtime/*.c"
         "${MH4N_REPO_ROOT}/tools/recomp/runtime/*.h" "${MH4N_REPO_ROOT}/tools/*.py")
    add_custom_command(
        OUTPUT "${MH4N_DETERMINISM_DATA_DIR}/svc_test.elf" "${MH4N_DETERMINISM_DATA_DIR}/svc_test_det.so"
        COMMAND "${Python3_EXECUTABLE}" "${MH4N_REPO_ROOT}/port/tests/build_svc_test.py"
                -o "${MH4N_DETERMINISM_DATA_DIR}" --program svc_test
        COMMAND "${Python3_EXECUTABLE}" -m recomp.build "${MH4N_DETERMINISM_DATA_DIR}" --code-library
                --opt O1 -j 2 ${determinism_cc_args} --native --deterministic -o svc_test_det.so
        WORKING_DIRECTORY "${MH4N_REPO_ROOT}/tools"
        DEPENDS "${MH4N_DETERMINISM_PROGRAM}" "${MH4N_REPO_ROOT}/port/tests/svc.h"
                "${MH4N_REPO_ROOT}/port/tests/build_svc_test.py" ${determinism_tool_sources}
        COMMENT "Building the deterministic mode's test program and its code library"
        VERBATIM)
    add_custom_target(mh4n_kernel_determinism_test_data ALL
        DEPENDS "${MH4N_DETERMINISM_DATA_DIR}/svc_test.elf" "${MH4N_DETERMINISM_DATA_DIR}/svc_test_det.so")
    target_compile_definitions(mh4n_kernel PUBLIC
        "MH4N_DETERMINISM_TEST_DATA_DIR=\"${MH4N_DETERMINISM_DATA_DIR}\""
        "MH4N_DETERMINISM_EXPECTED_DIR=\"${MH4N_REPO_ROOT}/port/tests/expected\"")
else()
    message(STATUS "mh4n_kernel: no deterministic test program (needs Python 3 and clang, ld.lld, "
                   "llvm-objcopy, llvm-readelf, and on Windows x86_64-w64-mingw32-clang): the cases of "
                   "determinism_test that run it will skip")
endif()
