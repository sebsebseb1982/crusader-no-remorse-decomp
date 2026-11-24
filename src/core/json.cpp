#include "core/json.hpp"

#include <cmath>
#include <cstdint>

namespace crusader {

// @orig none: port addition (JSON files)
const JsonValue* JsonValue::get(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

// @orig none: port addition (JSON files)
double JsonValue::number_at(std::string_view key, double fallback) const {
    const JsonValue* v = get(key);
    return v ? v->number_or(fallback) : fallback;
}

// @orig none: port addition (JSON files)
std::string JsonValue::string_at(std::string_view key, const std::string& fallback) const {
    const JsonValue* v = get(key);
    return v && v->is_string() ? v->string : fallback;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    // @orig none: port addition (JSON files)
    std::optional<JsonValue> document(std::string* error) {
        JsonValue v;
        skip();
        const bool ok = value(v, 0) && (skip(), at_end() || fail("text after the value"));
        if (!ok) {
            if (error) *error = where() + ": " + why_;
            return std::nullopt;
        }
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    bool at_end() const { return i_ >= s_.size(); }
    char peek() const { return at_end() ? '\0' : s_[i_]; }
    bool fail(const char* why) {
        if (why_.empty()) why_ = why;
        return false;
    }
    void skip() {
        while (!at_end() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return fail("unknown word");
        i_ += word.size();
        return true;
    }
    // @orig none: port addition (JSON files)
    std::string where() const {
        int line = 1, column = 1;
        for (std::size_t k = 0; k < i_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') ++line, column = 1;
            else ++column;
        }
        return "line " + std::to_string(line) + ", column " + std::to_string(column);
    }

    // @orig none: port addition (JSON files)
    bool value(JsonValue& v, int depth) {
        if (depth > kMaxDepth) return fail("nested too deep");
        switch (peek()) {
        case '{': return object(v, depth);
        case '[': return array(v, depth);
        case '"': v.type = JsonValue::Type::kString; return string(v.string);
        case 't': v.type = JsonValue::Type::kBool; v.boolean = true; return literal("true");
        case 'f': v.type = JsonValue::Type::kBool; v.boolean = false; return literal("false");
        case 'n': v.type = JsonValue::Type::kNull; return literal("null");
        default: v.type = JsonValue::Type::kNumber; return number(v.number);
        }
    }

    // @orig none: port addition (JSON files)
    bool object(JsonValue& v, int depth) {
        v.type = JsonValue::Type::kObject;
        ++i_;
        skip();
        if (peek() == '}') return ++i_, true;
        for (;;) {
            skip();
            if (peek() != '"') return fail("a key (a string) expected");
            std::string key;
            if (!string(key)) return false;
            skip();
            if (peek() != ':') return fail("':' expected");
            ++i_;
            skip();
            JsonValue member;
            if (!value(member, depth + 1)) return false;
            v.object.emplace_back(std::move(key), std::move(member));
            skip();
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == '}') return ++i_, true;
            return fail("',' or '}' expected");
        }
    }

    // @orig none: port addition (JSON files)
    bool array(JsonValue& v, int depth) {
        v.type = JsonValue::Type::kArray;
        ++i_;
        skip();
        if (peek() == ']') return ++i_, true;
        for (;;) {
            skip();
            JsonValue element;
            if (!value(element, depth + 1)) return false;
            v.array.push_back(std::move(element));
            skip();
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == ']') return ++i_, true;
            return fail("',' or ']' expected");
        }
    }

    // @orig none: port addition (JSON files)
    static void utf8(std::uint32_t c, std::string& out) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | c >> 12);
            out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | c >> 18);
            out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    // @orig none: port addition (JSON files)
    bool hex4(std::uint32_t& c) {
        if (i_ + 4 > s_.size()) return fail("\\u needs four hexadecimal digits");
        c = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            c <<= 4;
            if (h >= '0' && h <= '9') c |= h - '0';
            else if (h >= 'a' && h <= 'f') c |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') c |= h - 'A' + 10;
            else return fail("\\u needs four hexadecimal digits");
        }
        return true;
    }

    // @orig none: port addition (JSON files)
    bool string(std::string& out) {
        ++i_;  // the opening quote
        for (;;) {
            if (at_end()) return fail("unterminated string");
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_end()) return fail("unterminated string");
            switch (s_[i_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t code;
                if (!hex4(code)) return false;
                if (code >= 0xD800 && code < 0xDC00 && s_.substr(i_, 2) == "\\u") {
                    i_ += 2;
                    std::uint32_t low;
                    if (!hex4(low)) return false;
                    if (low >= 0xDC00 && low < 0xE000) code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8(code, out);
                break;
            }
            default: return fail("unknown escape in a string");
            }
        }
    }

    // A number as JSON writes it, read digit by digit (strtod would follow
    // the locale's decimal point).
    // @orig none: port addition (JSON files)
    bool number(double& out) {
        const std::size_t start = i_;
        bool negative = false;
        if (peek() == '-') negative = true, ++i_;
        if (!(peek() >= '0' && peek() <= '9')) {
            i_ = start;
            return fail("a value expected");
        }
        double mantissa = 0;
        while (peek() >= '0' && peek() <= '9') mantissa = mantissa * 10 + (s_[i_++] - '0');
        int exponent = 0;
        if (peek() == '.') {
            ++i_;
            if (!(peek() >= '0' && peek() <= '9')) return fail("a digit expected after '.'");
            while (peek() >= '0' && peek() <= '9') {
                mantissa = mantissa * 10 + (s_[i_++] - '0');
                --exponent;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++i_;
            bool minus = false;
            if (peek() == '+' || peek() == '-') minus = s_[i_++] == '-';
            if (!(peek() >= '0' && peek() <= '9')) return fail("a digit expected in the exponent");
            int e = 0;
            while (peek() >= '0' && peek() <= '9') e = e < 10000 ? e * 10 + (s_[i_++] - '0') : (++i_, e);
            exponent += minus ? -e : e;
        }
        // Divided rather than times a negative power: 0.65 is 65 / 100.
        out = exponent < 0 ? mantissa / std::pow(10.0, -exponent) : mantissa * std::pow(10.0, exponent);
        if (negative) out = -out;
        return true;
    }

    std::string_view s_;
    std::size_t i_ = 0;
    std::string why_;
};

}  // namespace

// @orig none: port addition (JSON files)
std::optional<JsonValue> parse_json(std::string_view text, std::string* error) {
    // A UTF-8 byte order mark, as some editors write it.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    return Parser(text).document(error);
}

}  // namespace crusader
