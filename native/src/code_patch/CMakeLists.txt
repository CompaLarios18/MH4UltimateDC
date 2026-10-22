# mh4n_code_patch: the code and exheader of a game folder patched from <user>/load/mods/<program id>/ as
# Azahar's NCCH loader does (N3-09, D72). The IPS and BPS patchers are the layeredfs module's (N3-10).
mh4n_module(SOURCES code_patch.cpp tool.cpp DEPENDS mh4n_layeredfs)
# Included as "code_patch/<name>.h" by the tool and the tests.
target_include_directories(mh4n_code_patch PUBLIC "${MH4N_SRC_DIR}")
