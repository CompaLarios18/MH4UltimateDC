// Command-line options that register themselves (N1-01).
//
//     MH4N_OPTION("runner", "--ms", "N", "run for N milliseconds of game time");   // takes a value
//     MH4N_OPTION("runner", "--headless", "", "no window");                         // a flag
//
// `scope` names the program ("runner", "capture-player", "ipc-replay", a tool's name). A program
// calls ParseOptions(scope, argc, argv) and sees the options every linked module registered for
// its scope. Accepted forms: `--name value`, `--name=value` (an option with an ARG) and `--name`
// (a flag); `--` ends the options; anything else that does not start with `--`, and a lone `-`,
// is a positional argument.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

#include "mh4n/registry.h"
#include "mh4n/types.h"

namespace mh4n {

struct OptionSpec {
    std::string scope;
    std::string name; // with the leading dashes
    std::string arg;  // empty for a flag
    std::string help;
};

namespace detail {
struct OptionRegistrar {
    OptionRegistrar(std::string_view scope, std::string_view name, std::string_view arg,
                    std::string_view help);
};
} // namespace detail

// Thrown by the typed getters when a given value does not parse.
class OptionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Options {
public:
    // False if parsing failed (an unknown option, a missing value, a value on a flag).
    bool ok() const {
        return error_.empty();
    }
    const std::string& error() const {
        return error_;
    }

    const std::vector<std::string>& positional() const {
        return positional_;
    }

    bool Has(std::string_view name) const;
    // The last value given; nullopt if the option was not given (a flag gives an empty value).
    std::optional<std::string> Get(std::string_view name) const;
    // Every value given, in order (empty if none).
    std::vector<std::string> All(std::string_view name) const;

    // Typed getters: the default when the option is absent, OptionError if the last value is
    // malformed. Integers take decimal or 0x hexadecimal.
    std::string GetString(std::string_view name, std::string default_value = {}) const;
    s64 GetInt(std::string_view name, s64 default_value = 0) const;
    u64 GetUInt(std::string_view name, u64 default_value = 0) const;
    double GetDouble(std::string_view name, double default_value = 0.0) const;

private:
    friend Options ParseOptions(std::string_view scope, const std::vector<std::string>& args);

    std::vector<std::string> positional_;
    std::map<std::string, std::vector<std::string>, std::less<>> values_;
    std::string error_;
};

// Parses argv[1..argc). On an error prints it and the usage to stderr and returns an Options
// whose ok() is false.
Options ParseOptions(std::string_view scope, int argc, const char* const* argv);
// The same for arguments without the program name, and without printing anything.
Options ParseOptions(std::string_view scope, const std::vector<std::string>& args);

// One line per option of `scope`, in name order: "  --name ARG   help".
std::string Usage(std::string_view scope);

} // namespace mh4n

#define MH4N_OPTION(scope, name, arg, help)                                                        \
    [[maybe_unused]] static const ::mh4n::detail::OptionRegistrar MH4N_UNIQUE(mh4n_option_) {      \
        scope, name, arg, help                                                                     \
    }
