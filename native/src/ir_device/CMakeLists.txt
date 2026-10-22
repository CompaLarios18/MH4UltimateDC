# mh4n_ir_device: the Circle Pad Pro as Azahar emulates it (ExtraHID, N3-08): the IR protocol of the
# device behind ir:USER's device 1, with no service, kernel or timer of its own (N3-12 plugs it in).
# The header is included as "ir_device/circle_pad_pro.h".
mh4n_module(SOURCES circle_pad_pro.cpp)
target_include_directories(mh4n_ir_device PUBLIC "${MH4N_SRC_DIR}")

# No fused multiply-add: the stick's 12-bit fields are Azahar's float arithmetic, whatever the host.
# Precedent: native/src/screen_layout/CMakeLists.txt.
if (NOT MSVC)
    set_source_files_properties(circle_pad_pro.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
