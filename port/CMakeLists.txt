# MH4U PC port: Azahar's 3DS core with the game's recompiled code as the CPU.
#
# This directory is built from inside Azahar's source tree (see azahar/README.md):
#   cmake <azahar> -DMH4U_PORT_DIR=<repo>/port -DMH4U_RECOMP_DIR=<repo>/build/recomp ...
# MH4U_RECOMP_DIR is the output of tools/recompile.py for the user's own dump; it is
# generated locally and never distributed.

set(MH4U_RECOMP_DIR "" CACHE PATH "tools/recompile.py output for the game (contains src/)")
set(MH4U_TEST_RECOMP_DIR "" CACHE PATH "tools/recompile.py output for the port's test program")
set(MH4U_RECOMP_OPT "-O1" CACHE STRING "Optimisation level for recompiled code")

get_filename_component(MH4U_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools" ABSOLUTE)
set(MH4U_RUNTIME_DIR "${MH4U_TOOLS_DIR}/recomp/runtime")

set(MH4U_BACKEND_SOURCES
    src/arm_recomp.cpp
    src/arm_recomp.h
    src/fiber.cpp
    src/fiber.h
)

# Recompiled code from `dir`, as a static library, plus a headless runner around it.
function(mh4u_add_program name dir)
    file(GLOB code CONFIGURE_DEPENDS "${dir}/src/*.c")
    if (NOT code)
        message(FATAL_ERROR "no recompiled code in ${dir}/src (run tools/recompile.py)")
    endif()
    add_library(${name}_code STATIC ${code})
    target_include_directories(${name}_code PRIVATE "${MH4U_RUNTIME_DIR}" "${dir}/src")
    target_compile_definitions(${name}_code PRIVATE RC_PAGED)
    if (MSVC)
        target_compile_options(${name}_code PRIVATE /fp:strict /w)
    else()
        target_compile_options(${name}_code PRIVATE ${MH4U_RECOMP_OPT} -ffp-contract=off
                               -fno-strict-aliasing -fwrapv -w)
    endif()

    # The backend sits in the executable so the code library can call back into it.
    add_executable(${name}_headless src/headless.cpp ${MH4U_BACKEND_SOURCES})
    target_include_directories(${name}_headless PRIVATE src "${MH4U_RUNTIME_DIR}")
    target_link_libraries(${name}_headless PRIVATE ${name}_code citra_core citra_common)
endfunction()

if (MH4U_RECOMP_DIR)
    mh4u_add_program(mh4u "${MH4U_RECOMP_DIR}")
endif()
if (MH4U_TEST_RECOMP_DIR)
    mh4u_add_program(mh4u_test "${MH4U_TEST_RECOMP_DIR}")
endif()
