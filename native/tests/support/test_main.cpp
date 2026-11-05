// The main of the test binaries that do not define MH4N_TEST_OWN_MAIN.
//
// MH4N_TEST_DURATIONS=1 (the Linux CI jobs, N0-42) makes stdout unbuffered, turns on doctest's `duration` option
// (each case's time when it ends; `--duration=false` on the command line still wins) and prints `case start: <name>`
// when a case starts, so that the case running when ctest kills the binary at its TIMEOUT is named in the captured
// output. tools/native/ctest_case_times.py reads those lines back from ctest's LastTest.log. The start lines are for
// the console reporter: with `-r=xml` or `-r=junit` they land inside that reporter's output. Without the variable the
// output is unchanged, `--list-reporters` included (main registers the listener only with it).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <ostream>

#include <doctest/doctest.h>

namespace {

bool DurationsOn() {
    const char* value = std::getenv("MH4N_TEST_DURATIONS");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// Prints `case start: <name>` through doctest's own stream (ContextOptions::cout: its order with doctest's lines, and
// --out). main registers it only while MH4N_TEST_DURATIONS=1. doctest prints a case's preamble and time only when the
// case ends.
struct CaseStartListener : public doctest::IReporter {
    std::ostream* out;

    explicit CaseStartListener(const doctest::ContextOptions& options) : out(options.cout) {}

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& data) override {
        if (out != nullptr) {
            *out << "case start: " << data.m_name << '\n';
        }
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

}  // namespace

int main(int argc, char** argv) {
    const bool durations = DurationsOn();
    if (durations) {
        // before anything writes to stdout: a piped stdout is buffered in blocks, and a kill loses the block
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::cout << std::unitbuf;
    }
    // An assertion that fails where it cannot unwind (a worker thread, a destructor) ends the test in
    // std::terminate. Flush what doctest printed first, so that the failing assertion reaches a CI log
    // (a piped stdout is buffered and is otherwise lost on abort).
    std::set_terminate([] {
        std::cout.flush();
        std::fflush(nullptr);
        std::abort();
    });
    if (!durations) {
        return doctest::Context(argc, argv).run();
    }
    doctest::registerReporter<CaseStartListener>("mh4n_case_start", 1, false);  // false: a listener, not a reporter
    doctest::Context context;
    context.setOption("duration", true);
    context.applyCommandLine(argc, argv);
    return context.run();
}
