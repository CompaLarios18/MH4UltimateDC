# mh4u_native_runner: only main and its glue; the runtime is the module native/src/runtime.
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
    # the runtime's own options are linked in and listed
    add_test(NAME runner_help_lists_runtime_options COMMAND mh4u_native_runner --help)
    set_tests_properties(runner_help_lists_runtime_options PROPERTIES
                         PASS_REGULAR_EXPRESSION "--init-time .*--trace-svc")
    # no PROGRAM, or one that cannot be loaded: exit status 2
    add_test(NAME runner_needs_a_program COMMAND mh4u_native_runner)
    set_tests_properties(runner_needs_a_program PROPERTIES WILL_FAIL TRUE)
    add_test(NAME runner_missing_program COMMAND mh4u_native_runner no-such-program.elf)
    set_tests_properties(runner_missing_program PROPERTIES WILL_FAIL TRUE)
endif()
