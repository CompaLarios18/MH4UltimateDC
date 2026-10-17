# One doctest binary per native/tests/<name>_test.cpp, linked against every module.
#
# A test file that defines MH4N_TEST_OWN_MAIN (a line `#define MH4N_TEST_OWN_MAIN`) supplies its
# own main (for subprocess tests); it still gets doctest's implementation. All others get a main
# that runs doctest.
add_library(mh4n_test_doctest STATIC support/doctest_impl.cpp)
target_link_libraries(mh4n_test_doctest PUBLIC doctest::doctest)
target_compile_options(mh4n_test_doctest PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)

add_library(mh4n_test_main STATIC support/test_main.cpp)
target_link_libraries(mh4n_test_main PUBLIC mh4n_test_doctest)
target_compile_options(mh4n_test_main PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)

file(GLOB test_sources CONFIGURE_DEPENDS LIST_DIRECTORIES false "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
list(SORT test_sources)
foreach (source IN LISTS test_sources)
    get_filename_component(test_name "${source}" NAME_WE)
    file(READ "${source}" text)
    add_executable(${test_name} "${source}")
    target_link_libraries(${test_name} PRIVATE mh4n::all mh4n_warnings)
    if (text MATCHES "(^|\n)[ \t]*#[ \t]*define[ \t]+MH4N_TEST_OWN_MAIN")
        target_link_libraries(${test_name} PRIVATE mh4n_test_doctest)
    else()
        target_link_libraries(${test_name} PRIVATE mh4n_test_main)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
endforeach()
