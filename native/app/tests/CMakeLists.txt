# One doctest binary per native/app/tests/<name>_test.cpp, linked against mh4n::app. Like
# native/tests: a file with a line `#define MH4N_TEST_OWN_MAIN` supplies its own main. (The test
# support libraries mh4n_test_main and mh4n_test_doctest come from native/tests, configured first.)
file(GLOB app_test_sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
list(SORT app_test_sources)
foreach (source IN LISTS app_test_sources)
    get_filename_component(test_name "${source}" NAME_WE)
    file(READ "${source}" text)
    add_executable(${test_name} "${source}")
    target_link_libraries(${test_name} PRIVATE mh4n::app mh4n_warnings)
    if (text MATCHES "(^|\n)[ \t]*#[ \t]*define[ \t]+MH4N_TEST_OWN_MAIN")
        target_link_libraries(${test_name} PRIVATE mh4n_test_doctest)
    else()
        target_link_libraries(${test_name} PRIVATE mh4n_test_main)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
endforeach()
