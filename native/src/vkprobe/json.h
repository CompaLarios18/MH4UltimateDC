// A small JSON reader for mh4n_vkprobe (N2-24): the subset `vulkaninfo --json` and the probe's own
// report use (objects, arrays, strings, numbers, booleans, null), with a nesting limit so that hostile
// or damaged input is an error and never a stack overflow. Nothing else in the tree needs it, so it
// lives with the probe.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mh4n::vkprobe {

struct JsonMember;

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    std::vector<JsonMember> object;  // members in file order

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsBool() const { return type == Type::Bool; }
    // The first member called `key`, or nullptr (also when this is not an object).
    const Json* Find(std::string_view key) const;
};

struct JsonMember {
    std::string key;
    Json value;
};

// The deepest nesting of arrays and objects the reader accepts.
inline constexpr int kJsonMaxDepth = 64;

// Parses every JSON document in `text` (one after the other, separated by white space only: the
// probe's `--json` writes one per device). False, with a message that says what and where, when the
// text holds no document, a document is invalid or truncated, or something other than white space
// follows one.
bool ParseJsonDocuments(std::string_view text, std::vector<Json>* documents, std::string* error);

// "text" as a JSON string literal, quotes included.
std::string JsonQuote(std::string_view text);

}  // namespace mh4n::vkprobe
