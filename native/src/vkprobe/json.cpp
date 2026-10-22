#include "vkprobe/json.h"

#include <cstdlib>
#include <cstring>

namespace mh4n::vkprobe {

const Json* Json::Find(std::string_view key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const JsonMember& member : object) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

namespace {

// A byte for an error message: the character when it is printable ASCII, else 0x..
std::string DescribeByte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u < 0x7F) {
        return std::string("'") + c + "'";
    }
    static const char* hex = "0123456789abcdef";
    return std::string("byte 0x") + hex[u >> 4] + hex[u & 15];
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    // At the end of the text (only white space left)?
    bool AtEnd() {
        SkipSpace();
        return pos_ >= text_.size();
    }

    bool ParseDocument(Json& out) {
        return ParseValue(out, 0);
    }

    const std::string& error() const { return error_; }

private:
    bool Fail(const std::string& message) {
        if (error_.empty()) {
            size_t line = 1;
            size_t column = 1;
            for (size_t i = 0; i < pos_ && i < text_.size(); ++i) {
                if (text_[i] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            error_ = message + " (line " + std::to_string(line) + ", column " + std::to_string(column) + ")";
        }
        return false;
    }
    bool Truncated(const char* what) {
        return Fail(std::string("the text ends inside ") + what + ": the file is truncated");
    }

    void SkipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool ParseValue(Json& out, int depth) {
        SkipSpace();
        if (pos_ >= text_.size()) {
            return Truncated("a value");
        }
        const char c = text_[pos_];
        switch (c) {
        case '{':
            return ParseObject(out, depth);
        case '[':
            return ParseArray(out, depth);
        case '"':
            out.type = Json::Type::String;
            return ParseString(out.string);
        case 't':
            return Literal("true", out, Json::Type::Bool, true);
        case 'f':
            return Literal("false", out, Json::Type::Bool, false);
        case 'n':
            return Literal("null", out, Json::Type::Null, false);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return ParseNumber(out);
            }
            return Fail("unexpected " + DescribeByte(c));
        }
    }

    bool Literal(const char* word, Json& out, Json::Type type, bool value) {
        const size_t n = std::strlen(word);
        if (text_.size() - pos_ < n) {
            // a prefix of the word at the end of the text is a truncation, anything else is garbage
            if (text_.substr(pos_) == std::string_view(word, text_.size() - pos_)) {
                return Truncated(word);
            }
            return Fail("invalid literal");
        }
        if (text_.substr(pos_, n) != word) {
            return Fail("invalid literal");
        }
        pos_ += n;
        out.type = type;
        out.boolean = value;
        return true;
    }

    bool ParseNumber(Json& out) {
        const size_t start = pos_;
        if (text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size()) {
            return Truncated("a number");
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && IsDigit(text_[pos_])) {
                ++pos_;
            }
        } else {
            return Fail("invalid number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size()) {
                return Truncated("a number");
            }
            if (!IsDigit(text_[pos_])) {
                return Fail("invalid number");
            }
            while (pos_ < text_.size() && IsDigit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size()) {
                return Truncated("a number");
            }
            if (!IsDigit(text_[pos_])) {
                return Fail("invalid number");
            }
            while (pos_ < text_.size() && IsDigit(text_[pos_])) {
                ++pos_;
            }
        }
        const std::string token(text_.substr(start, pos_ - start));
        out.type = Json::Type::Number;
        out.number = std::strtod(token.c_str(), nullptr);
        return true;
    }

    static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

    static void AppendUtf8(std::string& s, unsigned long cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned long* value) {
        if (text_.size() - pos_ < 4) {
            return Truncated("a string");
        }
        unsigned long v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + i];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<unsigned long>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned long>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned long>(c - 'A' + 10);
            } else {
                return Fail("invalid \\u escape");
            }
        }
        pos_ += 4;
        *value = v;
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // the opening quote
        out.clear();
        while (true) {
            if (pos_ >= text_.size()) {
                return Truncated("a string");
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                return Fail("a control character in a string");
            }
            if (c != '\\') {
                out += static_cast<char>(c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) {
                return Truncated("a string");
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
                unsigned long cp = 0;
                if (!Hex4(&cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && text_.size() - pos_ >= 6 && text_[pos_] == '\\' &&
                    text_[pos_ + 1] == 'u') {
                    const size_t save = pos_;
                    pos_ += 2;
                    unsigned long low = 0;
                    if (!Hex4(&low)) {
                        return false;
                    }
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        pos_ = save;
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;  // a lone surrogate
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                --pos_;
                return Fail("invalid escape \\" + DescribeByte(e) + " in a string");
            }
        }
    }

    bool ParseArray(Json& out, int depth) {
        if (depth >= kJsonMaxDepth) {
            return Fail("nesting deeper than " + std::to_string(kJsonMaxDepth));
        }
        ++pos_;  // [
        out.type = Json::Type::Array;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            out.array.emplace_back();
            if (!ParseValue(out.array.back(), depth + 1)) {
                return false;
            }
            SkipSpace();
            if (pos_ >= text_.size()) {
                return Truncated("an array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Json& out, int depth) {
        if (depth >= kJsonMaxDepth) {
            return Fail("nesting deeper than " + std::to_string(kJsonMaxDepth));
        }
        ++pos_;  // {
        out.type = Json::Type::Object;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipSpace();
            if (pos_ >= text_.size()) {
                return Truncated("an object");
            }
            if (text_[pos_] != '"') {
                return Fail("expected a string as the name of a member");
            }
            out.object.emplace_back();
            if (!ParseString(out.object.back().key)) {
                return false;
            }
            SkipSpace();
            if (pos_ >= text_.size()) {
                return Truncated("an object");
            }
            if (text_[pos_] != ':') {
                return Fail("expected ':'");
            }
            ++pos_;
            if (!ParseValue(out.object.back().value, depth + 1)) {
                return false;
            }
            SkipSpace();
            if (pos_ >= text_.size()) {
                return Truncated("an object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace

bool ParseJsonDocuments(std::string_view text, std::vector<Json>* documents, std::string* error) {
    documents->clear();
    // PowerShell 5.1's `>` writes UTF-16 with a byte order mark; say so instead of "unexpected byte 0xff"
    if (text.size() >= 2 && ((text[0] == '\xFF' && text[1] == '\xFE') || (text[0] == '\xFE' && text[1] == '\xFF'))) {
        *error = "this file is UTF-16 (PowerShell's > does that); save it as UTF-8";
        return false;
    }
    // a UTF-8 byte order mark, which some Windows tools write
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    Parser parser(text);
    while (!parser.AtEnd()) {
        Json doc;
        if (!parser.ParseDocument(doc)) {
            *error = parser.error();
            documents->clear();
            return false;
        }
        documents->push_back(std::move(doc));
    }
    if (documents->empty()) {
        *error = "no JSON document (the file is empty)";
        return false;
    }
    return true;
}

std::string JsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

}  // namespace mh4n::vkprobe
