# mh4u-native: only main and its glue; the app's code is in the modules next to this folder.
add_executable(mh4u-native main.cpp)
target_link_libraries(mh4u-native PRIVATE mh4n::app mh4n_warnings)
target_include_directories(mh4u-native PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(mh4u-native mh4n_version)

if (MH4N_TESTS)
    add_test(NAME app_help COMMAND mh4u-native --help)
    set_tests_properties(app_help PROPERTIES PASS_REGULAR_EXPRESSION "usage: mh4u-native")
    add_test(NAME app_version COMMAND mh4u-native --version)
    set_tests_properties(app_version PROPERTIES PASS_REGULAR_EXPRESSION "mh4u-native [^ ]+")
    add_test(NAME app_unknown_option COMMAND mh4u-native --no-such-option)
    set_tests_properties(app_unknown_option PROPERTIES WILL_FAIL TRUE)
endif()
