#include "mh4n/options.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

#include <fmt/format.h>

namespace mh4n {
namespace {

using OptionRegistry = Registry<OptionSpec>;

// "<scope>\t<name>": the registry's name order is then scope first, option name second.
std::string Key(std::string_view scope, std::string_view name) {
    std::string key(scope);
    key += '\t';
    key += name;
    return key;
}

[[noreturn]] void BadValue(std::string_view name, const std::string& value, const char* what) {
    throw OptionError(fmt::format("{}: '{}' is not {}", name, value, what));
}

template <class Int>
Int ParseInteger(std::string_view name, const std::string& text) {
    std::string_view digits = text;
    bool negative = false;
    if constexpr (std::is_signed_v<Int>) {
        if (!digits.empty() && (digits.front() == '-' || digits.front() == '+')) {
            negative = digits.front() == '-';
            digits.remove_prefix(1);
        }
    }
    int base = 10;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    // Parse the magnitude unsigned so that the minimum value works.
    using Unsigned = std::make_unsigned_t<Int>;
    Unsigned magnitude = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, base);
    if (digits.empty() || ec != std::errc() || end != digits.data() + digits.size()) {
        BadValue(name, text, "a number");
    }
    if constexpr (std::is_signed_v<Int>) {
        const Unsigned limit = static_cast<Unsigned>(std::numeric_limits<Int>::max());
        if (negative) {
            if (magnitude > limit + 1u) {
                BadValue(name, text, "in range");
            }
            return static_cast<Int>(Unsigned(0) - magnitude);
        }
        if (magnitude > limit) {
            BadValue(name, text, "in range");
        }
    }
    return static_cast<Int>(magnitude);
}

} // namespace

namespace detail {

OptionRegistrar::OptionRegistrar(std::string_view scope, std::string_view name,
                                 std::string_view arg, std::string_view help) {
    OptionRegistry::Instance().Add(Key(scope, name),
                                   OptionSpec{std::string(scope), std::string(name),
                                              std::string(arg), std::string(help)});
}

} // namespace detail

bool Options::Has(std::string_view name) const {
    return values_.find(name) != values_.end();
}

std::optional<std::string> Options::Get(std::string_view name) const {
    auto it = values_.find(name);
    if (it == values_.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second.back();
}

std::vector<std::string> Options::All(std::string_view name) const {
    auto it = values_.find(name);
    return it == values_.end() ? std::vector<std::string>() : it->second;
}

std::string Options::GetString(std::string_view name, std::string default_value) const {
    auto value = Get(name);
    return value ? *value : std::move(default_value);
}

s64 Options::GetInt(std::string_view name, s64 default_value) const {
    auto value = Get(name);
    return value ? ParseInteger<s64>(name, *value) : default_value;
}

u64 Options::GetUInt(std::string_view name, u64 default_value) const {
    auto value = Get(name);
    return value ? ParseInteger<u64>(name, *value) : default_value;
}

double Options::GetDouble(std::string_view name, double default_value) const {
    auto value = Get(name);
    if (!value) {
        return default_value;
    }
    errno = 0;
    char* end = nullptr;
    const double result = std::strtod(value->c_str(), &end);
    if (value->empty() || end != value->c_str() + value->size() || errno == ERANGE) {
        BadValue(name, *value, "a number");
    }
    return result;
}

Options ParseOptions(std::string_view scope, const std::vector<std::string>& args) {
    Options options;
    const auto& registry = OptionRegistry::Instance();
    bool only_positional = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (only_positional || arg.size() < 3 || arg.compare(0, 2, "--") != 0) {
            // "--" itself ends the options; a lone "-" and "-x" are positional
            if (!only_positional && arg == "--") {
                only_positional = true;
            } else {
                options.positional_.push_back(arg);
            }
            continue;
        }
        const std::size_t equals = arg.find('=');
        const std::string name = arg.substr(0, equals);
        const OptionSpec* spec = registry.Find(Key(scope, name));
        if (!spec) {
            options.error_ = fmt::format("unknown option {}", name);
            return options;
        }
        std::string value;
        if (spec->arg.empty()) {
            if (equals != std::string::npos) {
                options.error_ = fmt::format("{} takes no value", name);
                return options;
            }
        } else if (equals != std::string::npos) {
            value = arg.substr(equals + 1);
        } else if (i + 1 < args.size()) {
            value = args[++i];
        } else {
            options.error_ = fmt::format("{} needs a value ({})", name, spec->arg);
            return options;
        }
        options.values_[name].push_back(std::move(value));
    }
    return options;
}

Options ParseOptions(std::string_view scope, int argc, const char* const* argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    Options options = ParseOptions(scope, args);
    if (!options.ok()) {
        const std::string text = fmt::format("error: {}\noptions:\n{}", options.error(), Usage(scope));
        std::fwrite(text.data(), 1, text.size(), stderr);
    }
    return options;
}

std::string Usage(std::string_view scope) {
    std::vector<const OptionSpec*> specs;
    for (const auto& entry : OptionRegistry::Instance().All()) {
        if (entry.second.scope == scope) {
            specs.push_back(&entry.second);
        }
    }
    std::size_t width = 0;
    std::vector<std::string> left;
    for (const OptionSpec* spec : specs) {
        left.push_back(spec->arg.empty() ? spec->name : spec->name + " " + spec->arg);
        width = std::max(width, left.back().size());
    }
    std::string out;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        out += fmt::format("  {:<{}}  {}\n", left[i], width, specs[i]->help);
    }
    return out;
}

} // namespace mh4n
