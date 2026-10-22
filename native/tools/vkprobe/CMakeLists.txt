# mh4n_vkprobe: only main; the logic is the mh4n_vkprobe module (N2-24).
mh4n_program(mh4n_vkprobe_tool SOURCES main.cpp)
set_target_properties(mh4n_vkprobe_tool PROPERTIES OUTPUT_NAME mh4n_vkprobe)

if (MH4N_TESTS)
    add_test(NAME vkprobe_help COMMAND mh4n_vkprobe_tool --help)
    set_tests_properties(vkprobe_help PROPERTIES PASS_REGULAR_EXPRESSION "usage: mh4n_vkprobe")
endif()
