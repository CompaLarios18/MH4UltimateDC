# mh4n_platform: the hooks' platform (N1-20). port/src/hooks.cpp, the port's native code around game
# functions, is compiled into this module flat (no RC_PAGED: it reads guest memory through ctx->mem,
# as the code libraries of --native do), and NativePlatform gives it what the Azahar build's
# Azahar-build platform gives it: the game clock, the refresh rate (to the GSP's VBlank period through
# Runtime::SetRefreshRate), the user directory, the log, and local-only network sync until N4.
# The `started` boot step installs the hooks on the code library once it is loaded.
#
# hooks.cpp is the port's file (shared with the Azahar build) and is not edited here. A copy of
# native/ on its own (tests/test_native_cmake_layout.py) has no port/ around it: the module is then
# empty and registers nothing.
set(MH4N_REPO_ROOT "${MH4N_ROOT}/..")
set(MH4N_HOOKS_SOURCE "${MH4N_REPO_ROOT}/port/src/hooks.cpp")
set(MH4N_PLATFORM_HEADER "${MH4N_REPO_ROOT}/port/src/platform.h")
set(MH4N_SCHEDULE_HEADER "${MH4N_REPO_ROOT}/port/src/schedule.h")
set(MH4N_RECOMP_RUNTIME "${MH4N_REPO_ROOT}/tools/recomp/runtime")

if (EXISTS "${MH4N_HOOKS_SOURCE}" AND EXISTS "${MH4N_PLATFORM_HEADER}"
    AND EXISTS "${MH4N_SCHEDULE_HEADER}" AND EXISTS "${MH4N_RECOMP_RUNTIME}/recomp.h")
    set(mh4n_platform_sources platform_native.cpp "${MH4N_HOOKS_SOURCE}")
    set(mh4n_platform_available ON)
else()
    message(STATUS "mh4n_platform: port/src/hooks.cpp or recomp.h not found around native/: an empty module")
    set(mh4n_platform_sources unavailable.cpp)
    set(mh4n_platform_available OFF)
endif()

# hooks.cpp is the port's file, built for Azahar with its own warnings: it has one constant nothing
# reads (PANEL_TOUCH_ALLOWED, a documented address), which -Wall -Wextra -Werror refuses. Only that
# warning, only for that file; platform_native.cpp and everything else keep the full set.
if (mh4n_platform_available AND NOT MSVC)
    set_source_files_properties("${MH4N_HOOKS_SOURCE}" PROPERTIES COMPILE_OPTIONS -Wno-unused-const-variable)
endif()

mh4n_module(
    SOURCES ${mh4n_platform_sources}
    DEPENDS
        mh4n_runtime
        mh4n_cpu
    THIRD_PARTY
        mh4n_fmt
)

if (mh4n_platform_available)
    # hooks.h and platform.h are included as "hooks.h" and "platform.h"; recomp.h comes with mh4n_cpu.
    get_filename_component(MH4N_PORT_HEADERS_DIR "${MH4N_PLATFORM_HEADER}" DIRECTORY)
    target_include_directories(mh4n_platform PUBLIC "${MH4N_PORT_HEADERS_DIR}")
    # The platform's own header is included as "platform/platform_native.h".
    target_include_directories(mh4n_platform PUBLIC "${MH4N_SRC_DIR}")
endif()
