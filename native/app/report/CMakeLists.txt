# Crash files and tester reports of mh4u-native (N3-13, D69): report::Install, SetGameState, Make,
# Redact and Pending. Installed and wired in by N3-30. The crash handler is plain POSIX or Win32
# (no SDL, no ImGui) and the report reads the player's own files; nothing here sends anything.
mh4n_app_module(SOURCES
    crash.cpp
    crash_posix.cpp
    crash_win32.cpp
    make.cpp
    redact.cpp)

# report.h and the headers next to it are included as "report/report.h" by the other app modules
# and tests, and the config savegame's reader (the console's user name) comes from the cfg module.
target_include_directories(mh4n_app_report PUBLIC "${MH4N_APP_DIR}")

# the build's commit for the crash file (the header the version option of mh4u-native uses)
target_include_directories(mh4n_app_report PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(mh4n_app_report mh4n_version)
