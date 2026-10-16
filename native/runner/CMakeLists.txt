# mh4u_native_runner: only main and its glue (N1-09 fills it in).
mh4n_program(mh4u_native_runner SOURCES main.cpp)
target_include_directories(mh4u_native_runner PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(mh4u_native_runner mh4n_version)

if (MH4N_TESTS)
    add_test(NAME runner_help COMMAND mh4u_native_runner --help)
    set_tests_properties(runner_help PROPERTIES PASS_REGULAR_EXPRESSION "usage: mh4u_native_runner")
    add_test(NAME runner_version COMMAND mh4u_native_runner --version)
    set_tests_properties(runner_version PROPERTIES PASS_REGULAR_EXPRESSION "mh4u_native_runner [^ ]+")
    add_test(NAME runner_unknown_option COMMAND mh4u_native_runner --no-such-option)
    set_tests_properties(runner_unknown_option PROPERTIES WILL_FAIL TRUE)
endif()
