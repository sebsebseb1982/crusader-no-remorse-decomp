#pragma once
// Port addition: a small JSON reader for the port's own editable files (the
// ceiling lights, engine/world/ceiling_lights.hpp). Objects keep their keys
// in file order; numbers are read without the C locale (a decimal point is
// always '.').

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crusader {

struct JsonValue {
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = Type::kNull;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    bool is_number() const { return type == Type::kNumber; }
    bool is_string() const { return type == Type::kString; }
    bool is_array() const { return type == Type::kArray; }
    bool is_object() const { return type == Type::kObject; }

    // The member `key` of an object, else null.
    const JsonValue* get(std::string_view key) const;
    // This number, or `fallback` when it is not one.
    double number_or(double fallback) const { return is_number() ? number : fallback; }
    // Member `key`'s number, or `fallback`.
    double number_at(std::string_view key, double fallback) const;
    // Member `key`'s string, or `fallback`.
    std::string string_at(std::string_view key, const std::string& fallback) const;
};

// The value `text` holds (one value, white space round it allowed), or none,
// `error` then telling where and why ("line 3, column 7: ...").
std::optional<JsonValue> parse_json(std::string_view text, std::string* error = nullptr);

}  // namespace crusader
