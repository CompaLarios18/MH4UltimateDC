# mh4n_layeredfs: the mods folder's romfs/ and romfs_ext/ laid over the game's RomFS (N3-10), Azahar's
# LayeredFS as a file the fs service can serve; and the IPS and BPS patches it applies.
#
# The module includes the fs service's headers (../services/fs/) for FileBackend and PositionedFile and
# declares no DEPENDS on mh4n_services_fs: N3-29 makes that module use this one, and every program links
# all modules whole (mh4n::all), which resolves PositionedFile.
mh4n_module(SOURCES layeredfs.cpp patch.cpp tool.cpp)
# Included as "layeredfs/<name>.h" by the tool and the tests.
target_include_directories(mh4n_layeredfs PUBLIC "${MH4N_SRC_DIR}")
