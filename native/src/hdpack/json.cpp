#include "hdpack/json.h"

#include <cstdlib>

#include <fmt/format.h>

namespace mh4n::hdpack {

const Json* Json::Find(const std::string& key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    const Json* found = nullptr;
    for (const auto& member : object) {
        if (member.first == key) {
            found = &member.second;
        }
    }
    return found;
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool Parse(Json& out) {
        SkipSpace();
        if (!ParseValue(out, 0)) {
            return false;
        }
        SkipSpace();
        if (pos_ != text_.size()) {
            return Fail("trailing characters");
        }
        return true;
    }

    const std::string& error() const {
        return error_;
    }

private:
    bool Fail(const char* what) {
        if (error_.empty()) {
            error_ = fmt::format("{} at offset {}", what, pos_);
        }
        return false;
    }

    // Whitespace and, like Azahar's parser call (ignore_comments = true), // and /* */ comments.
    void SkipSpace() {
        if (pos_ == 0 && text_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            pos_ = 3; // a UTF-8 byte order mark
        }
        for (;;) {
            while (pos_ < text_.size() &&
                   (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                    text_[pos_] == '\r')) {
                ++pos_;
            }
            if (text_.compare(pos_, 2, "//") == 0) {
                while (pos_ < text_.size() && text_[pos_] != '\n') {
                    ++pos_;
                }
            } else if (text_.compare(pos_, 2, "/*") == 0) {
                const std::size_t close = text_.find("*/", pos_ + 2);
                pos_ = close == std::string::npos ? text_.size() : close + 2;
            } else {
                return;
            }
        }
    }

    bool Literal(const char* word) {
        const std::string w(word);
        if (text_.compare(pos_, w.size(), w) != 0) {
            return false;
        }
        pos_ += w.size();
        return true;
    }

    bool ParseValue(Json& out, int depth) {
        if (depth > kMaxDepth) {
            return Fail("nested too deeply");
        }
        if (pos_ >= text_.size()) {
            return Fail("unexpected end");
        }
        const char c = text_[pos_];
        if (c == '{') {
            return ParseObject(out, depth);
        }
        if (c == '[') {
            return ParseArray(out, depth);
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return ParseString(out.string);
        }
        if (c == 't' || c == 'f') {
            out.type = Json::Type::Bool;
            out.boolean = (c == 't');
            return Literal(out.boolean ? "true" : "false") || Fail("bad literal");
        }
        if (c == 'n') {
            out.type = Json::Type::Null;
            return Literal("null") || Fail("bad literal");
        }
        return ParseNumber(out);
    }

    bool ParseNumber(Json& out) {
        const char* begin = text_.c_str() + pos_;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin) {
            return Fail("unexpected character");
        }
        out.type = Json::Type::Number;
        out.number = value;
        pos_ += static_cast<std::size_t>(end - begin);
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool Hex4(unsigned& value) {
        if (pos_ + 4 > text_.size()) {
            return Fail("bad \\u escape");
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return Fail("bad \\u escape");
            }
        }
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_; // the opening quote
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                break;
            }
            const char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = 0;
                if (!Hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code < 0xDC00 && text_.compare(pos_, 2, "\\u") == 0) {
                    pos_ += 2;
                    unsigned low = 0;
                    if (!Hex4(low)) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, code);
                break;
            }
            default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseArray(Json& out, int depth) {
        out.type = Json::Type::Array;
        ++pos_;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            out.array.emplace_back();
            SkipSpace();
            if (!ParseValue(out.array.back(), depth + 1)) {
                return false;
            }
            SkipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Json& out, int depth) {
        out.type = Json::Type::Object;
        ++pos_;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return Fail("expected a string key");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return Fail("expected ':'");
            }
            ++pos_;
            SkipSpace();
            Json value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.object.push_back(JsonMember{std::move(key), std::move(value)});
            SkipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    std::string error_;
};

} // namespace

std::optional<Json> ParseJson(const std::string& text, std::string* error) {
    Parser parser(text);
    Json root;
    if (!parser.Parse(root)) {
        if (error) {
            *error = parser.error();
        }
        return std::nullopt;
    }
    return root;
}

} // namespace mh4n::hdpack
