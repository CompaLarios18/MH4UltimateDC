# The app's non-UI shared sources (port/app), compiled once for mh4u-native: the settings file,
# the input profile, the keyboard-and-mouse arithmetic, invite codes and the app's util/. The UI
# (port/app/ui) and the window come with the later WPs of N2b, each in a module of its own, and
# never compile these again. native_rooms.cpp is mh4u-native's stub of the network rooms (N4).
# (The paths are spelled out, not held in a variable, so that tools/native/check_native_deps.py
# sees which port/app sources native/app compiles.)
mh4n_app_module(SOURCES
    native_rooms.cpp
    ../../../port/app/config.cpp
    ../../../port/app/input.cpp
    ../../../port/app/invite.cpp
    ../../../port/app/kbm.cpp
    ../../../port/app/util/file.cpp
    ../../../port/app/util/log.cpp
    ../../../port/app/util/strings.cpp)
