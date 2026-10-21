// The main of the test binaries that do not define MH4N_TEST_OWN_MAIN.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>

#include <doctest/doctest.h>

int main(int argc, char** argv) {
    // An assertion that fails where it cannot unwind (a worker thread, a destructor) ends the test in
    // std::terminate. Flush what doctest printed first, so that the failing assertion reaches a CI log
    // (a piped stdout is buffered and is otherwise lost on abort).
    std::set_terminate([] {
        std::cout.flush();
        std::fflush(nullptr);
        std::abort();
    });
    return doctest::Context(argc, argv).run();
}
