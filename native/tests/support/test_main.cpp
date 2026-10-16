// The main of the test binaries that do not define MH4N_TEST_OWN_MAIN.
#include <doctest/doctest.h>

int main(int argc, char** argv) {
    return doctest::Context(argc, argv).run();
}
