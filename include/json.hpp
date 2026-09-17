#pragma once

// Read-only JSON DOM for task files.
//
// One pass over the file builds a tape of 16-byte nodes in document order;
// strings are views into the file buffer (decoded only if escaped) and numbers
// are parsed on access. Object members iterate in key order, like nlohmann's
// std::map-backed objects, so action numbering does not change.

#include <cctype>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json {

enum class Type : std::uint8_t { Null, False, True, Number, String, EscString, Array, Object };

struct Node {
    Type          type{Type::Null};
    std::uint32_t next{0};    // index just past this node's subtree
    std::uint32_t a{0};       // string/number: offset; container: element count
    std::uint32_t b{0};       // string/number: length
};

class Document;

class Value {
public:
    Value(const Document* d, std::uint32_t i) noexcept : d_(d), i_(i) {}

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null()   const noexcept { return type() == Type::Null; }
    [[nodiscard]] bool is_number() const noexcept { return type() == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept
        { return type() == Type::String || type() == Type::EscString; }
    [[nodiscard]] bool is_array()  const noexcept { return type() == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type() == Type::Object; }

    [[nodiscard]] std::string_view str() const;
    [[nodiscard]] double           number() const;

    template <class T> [[nodiscard]] T get() const;

    [[nodiscard]] std::size_t size()  const noexcept;
    [[nodiscard]] bool        empty() const noexcept { return size() == 0; }

    [[nodiscard]] Value operator[](std::size_t k) const;
    [[nodiscard]] Value at(std::string_view key) const;
    [[nodiscard]] bool  contains(std::string_view key) const noexcept;

    // Object members, sorted by key.
    [[nodiscard]] std::vector<std::pair<std::string_view, Value>> items() const;

    class iterator {
    public:
        iterator(const Document* d, std::uint32_t i) noexcept : d_(d), i_(i) {}
        Value operator*() const noexcept { return {d_, i_}; }
        iterator& operator++() noexcept;
        bool operator!=(const iterator& o) const noexcept { return i_ != o.i_; }
    private:
        const Document* d_;
        std::uint32_t   i_;
    };

    // Array elements (object values, for objects).
    [[nodiscard]] iterator begin() const noexcept;
    [[nodiscard]] iterator end()   const noexcept;

private:
    [[nodiscard]] const Node& node() const noexcept;
    [[nodiscard]] Value find(std::string_view key) const noexcept;

    const Document* d_;
    std::uint32_t   i_;
};

class Document {
public:
    [[nodiscard]] static Document parse_file(const std::string& path);
    [[nodiscard]] static Document parse(std::string text);

    [[nodiscard]] Value root() const noexcept { return {this, 0}; }

private:
    friend class Value;
    void parse_value();
    void parse_string(Node& n);
    [[noreturn]] void fail(const char* what) const;

    std::string       text_;
    std::string       decoded_;
    std::vector<Node> tape_;
    std::size_t       pos_{0};
};

// Value inline members.

inline const Node& Value::node() const noexcept { return d_->tape_[i_]; }
inline Type Value::type() const noexcept { return node().type; }

inline std::string_view Value::str() const {
    const Node& n = node();
    if (n.type == Type::String)    return {d_->text_.data() + n.a, n.b};
    if (n.type == Type::EscString) return {d_->decoded_.data() + n.a, n.b};
    throw std::runtime_error("json: expected string");
}

inline double Value::number() const {
    const Node& n = node();
    if (n.type != Type::Number) throw std::runtime_error("json: expected number");
    double v = 0;
    const char* p = d_->text_.data() + n.a;
    std::from_chars(p, p + n.b, v);
    return v;
}

template <> inline std::string Value::get<std::string>() const { return std::string(str()); }
template <> inline double      Value::get<double>()      const { return number(); }
template <> inline bool        Value::get<bool>()        const {
    if (type() == Type::True)  return true;
    if (type() == Type::False) return false;
    throw std::runtime_error("json: expected boolean");
}

inline std::size_t Value::size() const noexcept {
    const Node& n = node();
    return (n.type == Type::Array || n.type == Type::Object) ? n.a : 0;
}

inline Value::iterator& Value::iterator::operator++() noexcept {
    const auto& tape = d_->tape_;
    i_ = tape[i_].next;
    return *this;
}

inline Value::iterator Value::begin() const noexcept {
    const Node& n = node();
    if (n.type == Type::Array) return {d_, i_ + 1};
    return {d_, n.next};   // objects iterate through items()
}

inline Value::iterator Value::end() const noexcept { return {d_, node().next}; }

inline Value Value::operator[](std::size_t k) const {
    if (type() != Type::Array || k >= size()) throw std::runtime_error("json: index out of range");
    std::uint32_t i = i_ + 1;
    while (k--) i = d_->tape_[i].next;
    return {d_, i};
}

inline Value Value::find(std::string_view key) const noexcept {
    const Node& n = node();
    if (n.type != Type::Object) return {d_, UINT32_MAX};
    for (std::uint32_t i = i_ + 1, k = 0; k < n.a; ++k) {
        const Value kv{d_, i};
        const std::uint32_t v = d_->tape_[i].next;
        if (kv.str() == key) return {d_, v};
        i = d_->tape_[v].next;
    }
    return {d_, UINT32_MAX};
}

inline bool Value::contains(std::string_view key) const noexcept {
    return find(key).i_ != UINT32_MAX;
}

inline Value Value::at(std::string_view key) const {
    Value v = find(key);
    if (v.i_ == UINT32_MAX) throw std::runtime_error("json: missing key '" + std::string(key) + "'");
    return v;
}

} // namespace json
