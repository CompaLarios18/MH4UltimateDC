# mh4n_screen_layout: the arithmetic of the app's screen layouts and of touches on them (N2b-04),
# Azahar's layout functions as the port's patch has them. Pure arithmetic: no GPU, no SDL.
#
# Port::ScreenOverlay is declared in the port's platform.h (standard library only). A copy of
# native/ on its own (tests/test_native_cmake_layout.py) has no port/ around it: the module is then
# empty (its test skips itself).
set(MH4N_PLATFORM_HEADER "${MH4N_ROOT}/../port/src/platform.h")

if (EXISTS "${MH4N_PLATFORM_HEADER}")
    set(mh4n_screen_layout_sources screen_layout.cpp touch.cpp)
    set(mh4n_screen_layout_available ON)
else()
    message(STATUS "mh4n_screen_layout: port/src/platform.h not found around native/: an empty module")
    set(mh4n_screen_layout_sources unavailable.cpp)
    set(mh4n_screen_layout_available OFF)
endif()

mh4n_module(SOURCES ${mh4n_screen_layout_sources})

# No fused multiply-add: the rectangles and touch points are Azahar's floats (the oracle is built the
# same way), whatever the host. Precedent: native/src/cpu/interp/CMakeLists.txt.
if (mh4n_screen_layout_available AND NOT MSVC)
    set_source_files_properties(screen_layout.cpp touch.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

if (mh4n_screen_layout_available)
    # platform.h is included as "platform.h"; the module's own header as "screen_layout/screen_layout.h".
    get_filename_component(MH4N_PORT_HEADERS_DIR "${MH4N_PLATFORM_HEADER}" DIRECTORY)
    target_include_directories(mh4n_screen_layout PUBLIC "${MH4N_PORT_HEADERS_DIR}")
    target_include_directories(mh4n_screen_layout PUBLIC "${MH4N_SRC_DIR}")
endif()
