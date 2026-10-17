# mh4n_cpu: the code host (N1-04): code library loading, guest threads on fibers and the callbacks
# of recomp.h. N1-28's interpreter goes in native/src/cpu/interp/ as a module of its own.
set(MH4N_REPO_ROOT "${MH4N_ROOT}/..")
set(MH4N_FIBER_SOURCE "${MH4N_REPO_ROOT}/port/src/fiber.cpp")
set(MH4N_RECOMP_RUNTIME "${MH4N_REPO_ROOT}/tools/recomp/runtime")

# The module needs two files of the repository around native/: the fibers (the port's own, shared
# with its Azahar build) and recomp.h (the runtime contract of the code libraries). A copy of
# native/ on its own (tests/test_native_cmake_layout.py builds one) has neither and gets an empty
# module; cpu_test.cpp then has nothing to test.
if (EXISTS "${MH4N_FIBER_SOURCE}" AND EXISTS "${MH4N_RECOMP_RUNTIME}/recomp.h")
    set(mh4n_cpu_sources code_host.cpp code_library.cpp "${MH4N_FIBER_SOURCE}")
else()
    message(STATUS "mh4n_cpu: fiber.cpp or recomp.h not found around native/: an empty module")
    set(mh4n_cpu_sources unavailable.cpp)
endif()

mh4n_module(
    SOURCES ${mh4n_cpu_sources}
    DEPENDS
        mh4n_memory
    THIRD_PARTY
        mh4n_fmt
        ${CMAKE_DL_LIBS}
)
# fiber.h, and recomp.h also for the files that call the host: tests and the interpreter.
get_filename_component(MH4N_FIBER_DIR "${MH4N_FIBER_SOURCE}" DIRECTORY)
target_include_directories(mh4n_cpu PRIVATE "${MH4N_FIBER_DIR}")
target_include_directories(mh4n_cpu SYSTEM PUBLIC "${MH4N_RECOMP_RUNTIME}")

# ---- the test program and its code libraries (native/tests/cpu_test.cpp) ---------------------------
# port/tests/codehost_test.c is built into a 3DS ELF and recompiled, then into three code
# libraries, one per rc_build_mode, under <build>/test-data/codehost. The test finds the folder
# through MH4N_CPU_TEST_DATA_DIR and skips the cases that need it when it is empty. The step needs
# Python 3 and the clang tools that port/tests/build_svc_test.py calls. On Windows the libraries
# are built with llvm-mingw's x86_64-w64-mingw32-clang (--cc), which makes PE DLLs; they keep the
# .so names cpu_test.cpp looks for (LoadLibraryExW takes any file name). Without that compiler the
# step is left out there.
set(MH4N_CODEHOST_DATA_DIR "${CMAKE_BINARY_DIR}/test-data/codehost")
target_compile_definitions(mh4n_cpu PUBLIC "MH4N_CPU_TEST_DATA_DIR=\"${MH4N_CODEHOST_DATA_DIR}\"")

find_package(Python3 QUIET COMPONENTS Interpreter)
find_program(MH4N_TOOL_CLANG NAMES clang)
find_program(MH4N_TOOL_LLD NAMES ld.lld)
find_program(MH4N_TOOL_OBJCOPY NAMES llvm-objcopy)
find_program(MH4N_TOOL_READELF NAMES llvm-readelf)
set(MH4N_CODEHOST_PROGRAM "${MH4N_REPO_ROOT}/port/tests/codehost_test.c")
set(codehost_cc_args "")
set(codehost_have_cc TRUE)
if (WIN32)
    find_program(MH4N_TOOL_MINGW_CLANG NAMES x86_64-w64-mingw32-clang)
    if (MH4N_TOOL_MINGW_CLANG)
        set(codehost_cc_args --cc "${MH4N_TOOL_MINGW_CLANG}")
    else()
        set(codehost_have_cc FALSE)
    endif()
endif()
if (codehost_have_cc AND Python3_FOUND AND MH4N_TOOL_CLANG AND MH4N_TOOL_LLD AND MH4N_TOOL_OBJCOPY
    AND MH4N_TOOL_READELF AND EXISTS "${MH4N_CODEHOST_PROGRAM}")
    file(GLOB_RECURSE codehost_tool_sources CONFIGURE_DEPENDS
         "${MH4N_REPO_ROOT}/tools/recomp/*.py" "${MH4N_REPO_ROOT}/tools/recomp/runtime/*.c"
         "${MH4N_REPO_ROOT}/tools/recomp/runtime/*.h" "${MH4N_REPO_ROOT}/tools/*.py")
    set(codehost_libraries
        "${MH4N_CODEHOST_DATA_DIR}/codehost_native.so"
        "${MH4N_CODEHOST_DATA_DIR}/codehost_native_det.so"
        "${MH4N_CODEHOST_DATA_DIR}/codehost_paged.so")
    set(codehost_build_library
        "${Python3_EXECUTABLE}" -m recomp.build "${MH4N_CODEHOST_DATA_DIR}" --code-library
        --opt O1 -j 2 ${codehost_cc_args})
    add_custom_command(
        OUTPUT "${MH4N_CODEHOST_DATA_DIR}/codehost_test.elf" ${codehost_libraries}
        COMMAND "${Python3_EXECUTABLE}" "${MH4N_REPO_ROOT}/port/tests/build_svc_test.py"
                -o "${MH4N_CODEHOST_DATA_DIR}" --program codehost_test
        COMMAND ${codehost_build_library} --native -o codehost_native.so
        COMMAND ${codehost_build_library} --native --deterministic -o codehost_native_det.so
        COMMAND ${codehost_build_library} -o codehost_paged.so
        WORKING_DIRECTORY "${MH4N_REPO_ROOT}/tools"
        DEPENDS "${MH4N_CODEHOST_PROGRAM}" "${MH4N_REPO_ROOT}/port/tests/svc.h"
                "${MH4N_REPO_ROOT}/port/tests/build_svc_test.py" ${codehost_tool_sources}
        COMMENT "Building the code host's test program and its code libraries"
        VERBATIM)
    add_custom_target(mh4n_cpu_test_data ALL
        DEPENDS "${MH4N_CODEHOST_DATA_DIR}/codehost_test.elf" ${codehost_libraries})
else()
    message(STATUS "mh4n_cpu: no test program (needs Python 3 and clang, ld.lld, llvm-objcopy, "
                   "llvm-readelf, and on Windows x86_64-w64-mingw32-clang): the cases of cpu_test that run it will skip")
endif()
