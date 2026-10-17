// A small JSON reader for a pack's pack.json (N2-17): objects, arrays, strings, numbers,
// booleans and null, nothing else. Azahar reads the same file with nlohmann::json
// (CustomTexManager::ReadConfig); the keys the pre-processor uses are "options" and "textures".
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mh4n::hdpack {

struct JsonMember;

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    // Members in file order; a repeated key keeps its last value, like nlohmann::json.
    std::vector<JsonMember> object;

    bool IsObject() const {
        return type == Type::Object;
    }
    bool IsArray() const {
        return type == Type::Array;
    }
    bool IsString() const {
        return type == Type::String;
    }
    bool IsBool() const {
        return type == Type::Bool;
    }
    const Json* Find(const std::string& key) const;
};

struct JsonMember {
    std::string first;  // the key
    Json second;        // the value
};

// Parses `text`; nullopt (and a message in *error) when it is not valid JSON.
std::optional<Json> ParseJson(const std::string& text, std::string* error = nullptr);

} // namespace mh4n::hdpack
