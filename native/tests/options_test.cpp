// mh4n/options.h: MH4N_OPTION, ParseOptions, Usage.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mh4n/options.h"

using namespace mh4n;

MH4N_OPTION("test-opts", "--flag", "", "a flag");
MH4N_OPTION("test-opts", "--ms", "N", "milliseconds");
MH4N_OPTION("test-opts", "--rate", "X", "a real number");
MH4N_OPTION("test-opts", "--file", "PATH", "repeatable");
MH4N_OPTION("test-opts", "--aaa", "V", "sorts first");
MH4N_OPTION("other-scope", "--flag", "", "the same name in another scope is fine");
MH4N_OPTION("other-scope", "--only-other", "", "only in the other scope");

namespace {
Options Parse(std::vector<std::string> args) {
    return ParseOptions("test-opts", args);
}
} // namespace

TEST_CASE("positional arguments and options") {
    const Options o = Parse({"one", "--flag", "two", "--ms", "250", "-", "-x"});
    REQUIRE(o.ok());
    CHECK(o.positional() == std::vector<std::string>{"one", "two", "-", "-x"});
    CHECK(o.Has("--flag"));
    CHECK(o.Has("--ms"));
    CHECK_FALSE(o.Has("--rate"));
    CHECK_FALSE(o.Has("--not-registered"));
    CHECK(o.Get("--ms") == "250");
    CHECK_FALSE(o.Get("--rate").has_value());
    CHECK(o.Get("--flag") == ""); // a flag has an empty value
}

TEST_CASE("--name=value and --name value are the same") {
    const Options a = Parse({"--ms=100"});
    const Options b = Parse({"--ms", "100"});
    CHECK(a.GetInt("--ms") == 100);
    CHECK(b.GetInt("--ms") == 100);
    // a value may start with dashes, or be empty
    CHECK(Parse({"--file", "--weird"}).Get("--file") == "--weird");
    CHECK(Parse({"--file="}).Has("--file"));
    CHECK(Parse({"--file="}).Get("--file") == "");
    // and contain '='
    CHECK(Parse({"--file=a=b"}).Get("--file") == "a=b");
}

TEST_CASE("Get is the last value, All is every value") {
    const Options o = Parse({"--file", "a", "--flag", "--file=b", "--file", "c"});
    CHECK(o.Get("--file") == "c");
    CHECK(o.All("--file") == std::vector<std::string>{"a", "b", "c"});
    CHECK(o.All("--ms").empty());
    CHECK(o.All("--flag").size() == 1);
}

TEST_CASE("-- ends the options") {
    const Options o = Parse({"--flag", "--", "--ms", "--", "x"});
    REQUIRE(o.ok());
    CHECK(o.Has("--flag"));
    CHECK_FALSE(o.Has("--ms"));
    CHECK(o.positional() == std::vector<std::string>{"--ms", "--", "x"});
}

TEST_CASE("typed getters") {
    const Options o = Parse({"--ms", "0x10", "--rate", "-2.5"});
    CHECK(o.GetInt("--ms") == 16);
    CHECK(o.GetUInt("--ms") == 16u);
    CHECK(o.GetDouble("--rate") == doctest::Approx(-2.5));
    CHECK(o.GetString("--ms") == "0x10");
    // defaults when absent
    CHECK(o.GetInt("--aaa", 7) == 7);
    CHECK(o.GetUInt("--aaa", 8) == 8u);
    CHECK(o.GetDouble("--aaa", 1.5) == doctest::Approx(1.5));
    CHECK(o.GetString("--aaa", "dflt") == "dflt");
    // limits
    CHECK(Parse({"--ms=-9223372036854775808"}).GetInt("--ms") == INT64_MIN);
    CHECK(Parse({"--ms=9223372036854775807"}).GetInt("--ms") == INT64_MAX);
    CHECK(Parse({"--ms=18446744073709551615"}).GetUInt("--ms") == UINT64_MAX);
}

TEST_CASE("malformed numbers throw OptionError") {
    CHECK_THROWS_AS(Parse({"--ms=abc"}).GetInt("--ms"), OptionError);
    CHECK_THROWS_AS(Parse({"--ms=12x"}).GetInt("--ms"), OptionError);
    CHECK_THROWS_AS(Parse({"--ms="}).GetInt("--ms"), OptionError);
    CHECK_THROWS_AS(Parse({"--ms=-1"}).GetUInt("--ms"), OptionError);
    CHECK_THROWS_AS(Parse({"--ms=9223372036854775808"}).GetInt("--ms"), OptionError);
    CHECK_THROWS_AS(Parse({"--ms=18446744073709551616"}).GetUInt("--ms"), OptionError);
    CHECK_THROWS_AS(Parse({"--rate=1.5x"}).GetDouble("--rate"), OptionError);
    CHECK_THROWS_AS(Parse({"--rate="}).GetDouble("--rate"), OptionError);
    try {
        Parse({"--ms=abc"}).GetInt("--ms");
    } catch (const OptionError& e) {
        CHECK(std::string(e.what()) == "--ms: 'abc' is not a number");
    }
}

TEST_CASE("errors") {
    SUBCASE("an unknown option") {
        const Options o = Parse({"--nope"});
        CHECK_FALSE(o.ok());
        CHECK(o.error() == "unknown option --nope");
    }
    SUBCASE("an option of another scope is unknown") {
        CHECK_FALSE(Parse({"--only-other"}).ok());
        CHECK(ParseOptions("other-scope", std::vector<std::string>{"--only-other"}).ok());
    }
    SUBCASE("a missing value") {
        const Options o = Parse({"--flag", "--ms"});
        CHECK_FALSE(o.ok());
        CHECK(o.error() == "--ms needs a value (N)");
    }
    SUBCASE("a value given to a flag") {
        const Options o = Parse({"--flag=1"});
        CHECK_FALSE(o.ok());
        CHECK(o.error() == "--flag takes no value");
    }
    SUBCASE("argc/argv: skips argv[0]") {
        const char* argv[] = {"prog", "--ms", "5", "file"};
        const Options o = ParseOptions("test-opts", 4, argv);
        CHECK(o.ok());
        CHECK(o.GetInt("--ms") == 5);
        CHECK(o.positional() == std::vector<std::string>{"file"});
    }
}

TEST_CASE("Usage lists the options of one scope in name order") {
    const std::string usage = Usage("test-opts");
    const std::string expected =
        "  --aaa V      sorts first\n"
        "  --file PATH  repeatable\n"
        "  --flag       a flag\n"
        "  --ms N       milliseconds\n"
        "  --rate X     a real number\n";
    CHECK(usage == expected);
    CHECK(Usage("other-scope") ==
          "  --flag        the same name in another scope is fine\n"
          "  --only-other  only in the other scope\n");
    CHECK(Usage("no-such-scope").empty());
}
