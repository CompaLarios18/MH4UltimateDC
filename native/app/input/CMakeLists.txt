# Input of mh4u-native (N3-07, docs/native/input.md): controllers, the keyboard and mouse, the
# mouse camera, as one mh4n::InputState for the live input source. The app's shared sources it
# builds on (config.cpp, input.cpp, kbm.cpp) are compiled once by native/app/shared: link to it,
# never compile them again. No Azahar and no SDL2 header here (tools/native/check_native_deps.py).
mh4n_app_module(SOURCES
    azahar_rules.cpp
    gamepads_sdl3.cpp
    input_mapper.cpp
    DEPENDS mh4n_app_shared mh4n_app_host)
# the module's headers as "input/input_mapper.h" (the app's shared headers, port/app, have an
# input.h of their own)
target_include_directories(mh4n_app_input PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
