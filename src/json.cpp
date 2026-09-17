#include "json.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <cstdio>
#include <memory>

namespace json {

std::vector<std::pair<std::string_view, Value>> Value::members() const {
    std::vector<std::pair<std::string_view, Value>> out;
    const Node& n = node();
    if (n.type != Type::Object) return out;
    out.reserve(n.a);
    for (std::uint32_t i = i_ + 1, k = 0; k < n.a; ++k) {
        const std::uint32_t v = d_->tape_[i].next;
        out.emplace_back(Value{d_, i}.str(), Value{d_, v});
        i = d_->tape_[v].next;
    }
    return out;
}

std::vector<std::pair<std::string_view, Value>> Value::items() const {
    std::vector<std::pair<std::string_view, Value>> out;
    const Node& n = node();
    if (n.type != Type::Object) return out;
    out.reserve(n.a);
    for (std::uint32_t i = i_ + 1, k = 0; k < n.a; ++k) {
        const std::uint32_t v = d_->tape_[i].next;
        out.emplace_back(Value{d_, i}.str(), Value{d_, v});
        i = d_->tape_[v].next;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& x, const auto& y) { return x.first < y.first; });
    return out;
}

Document Document::parse_file(const std::string& path) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!f) throw std::runtime_error("Cannot open JSON file: " + path);
    std::string text;
    std::fseek(f.get(), 0, SEEK_END);
    const long len = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    if (len < 0) throw std::runtime_error("Cannot read JSON file: " + path);
    text.resize(static_cast<std::size_t>(len));
    if (std::fread(text.data(), 1, text.size(), f.get()) != text.size())
        throw std::runtime_error("Cannot read JSON file: " + path);
    return parse(std::move(text));
}

Document Document::parse(std::string text) {
    if (text.size() >= UINT32_MAX) throw std::runtime_error("json: file too large");
    Document d;
    d.text_ = std::move(text);
    d.tape_.reserve(d.text_.size() / 8);
    d.parse_value();
    while (d.pos_ < d.text_.size() && std::isspace(static_cast<unsigned char>(d.text_[d.pos_])))
        ++d.pos_;
    if (d.pos_ != d.text_.size()) d.fail("trailing characters");
    return d;
}

void Document::fail(const char* what) const {
    throw std::runtime_error(std::string("json: ") + what + " at byte " + std::to_string(pos_));
}

namespace {

inline bool is_ws(char c) noexcept { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// Advances past whitespace; task files are indented, so runs are long.
inline std::size_t skip_ws(const char* p, std::size_t pos, std::size_t end) noexcept {
#if defined(__SSE2__)
    const __m128i sp = _mm_set1_epi8(' '), nl = _mm_set1_epi8('\n'),
                  cr = _mm_set1_epi8('\r'), tb = _mm_set1_epi8('\t');
    while (pos + 16 <= end) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + pos));
        const __m128i ws = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, sp), _mm_cmpeq_epi8(x, nl)),
                                        _mm_or_si128(_mm_cmpeq_epi8(x, cr), _mm_cmpeq_epi8(x, tb)));
        const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(ws));
        if (mask != 0xFFFF) return pos + static_cast<std::size_t>(std::countr_one(mask));
        pos += 16;
    }
#endif
    while (pos < end && is_ws(p[pos])) ++pos;
    return pos;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

void Document::parse_string(Node& n) {
    const std::size_t start = ++pos_;   // past the opening quote
    const std::size_t end   = text_.size();

    const char* q = static_cast<const char*>(std::memchr(text_.data() + start, '"', end - start));
    if (!q) fail("unterminated string");
    std::size_t p = static_cast<std::size_t>(q - text_.data());
    if (const char* bs = static_cast<const char*>(std::memchr(text_.data() + start, '\\', p - start)))
        p = static_cast<std::size_t>(bs - text_.data());

    if (text_[p] == '"') {   // no escapes: view into the buffer
        n.type = Type::String;
        n.a = static_cast<std::uint32_t>(start);
        n.b = static_cast<std::uint32_t>(p - start);
        pos_ = p + 1;
        return;
    }

    n.type = Type::EscString;
    n.a = static_cast<std::uint32_t>(decoded_.size());
    decoded_.append(text_, start, p - start);
    const auto hex4 = [&](std::size_t at) -> std::uint32_t {
        if (at + 4 > end) fail("bad \\u escape");
        std::uint32_t v = 0;
        const auto r = std::from_chars(text_.data() + at, text_.data() + at + 4, v, 16);
        if (r.ptr != text_.data() + at + 4) fail("bad \\u escape");
        return v;
    };
    while (true) {
        if (p >= end) fail("unterminated string");
        const char c = text_[p];
        if (c == '"') break;
        if (c != '\\') { decoded_.push_back(c); ++p; continue; }
        if (++p >= end) fail("unterminated string");
        switch (text_[p]) {
            case '"':  decoded_.push_back('"');  break;
            case '\\': decoded_.push_back('\\'); break;
            case '/':  decoded_.push_back('/');  break;
            case 'b':  decoded_.push_back('\b'); break;
            case 'f':  decoded_.push_back('\f'); break;
            case 'n':  decoded_.push_back('\n'); break;
            case 'r':  decoded_.push_back('\r'); break;
            case 't':  decoded_.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = hex4(p + 1);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < end &&
                    text_[p + 1] == '\\' && text_[p + 2] == 'u') {
                    const std::uint32_t lo = hex4(p + 3);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                append_utf8(decoded_, cp);
                break;
            }
            default: fail("bad escape");
        }
        ++p;
    }
    n.b = static_cast<std::uint32_t>(decoded_.size() - n.a);
    pos_ = p + 1;
}

// Iterative over containers: an explicit stack of open container indices keeps
// deeply nested input from exhausting the call stack.
void Document::parse_value() {
    const std::size_t end = text_.size();
    std::vector<std::uint32_t> open;   // tape indices of unfinished containers

    const auto skip_ws = [&] { pos_ = json::skip_ws(text_.data(), pos_, end); };

    for (;;) {
        skip_ws();
        if (pos_ >= end) fail("unexpected end of input");

        const auto idx = static_cast<std::uint32_t>(tape_.size());
        tape_.emplace_back();
        const char c = text_[pos_];

        bool opened = false;
        switch (c) {
            case '{':
            case '[':
                tape_[idx].type = c == '{' ? Type::Object : Type::Array;
                ++pos_;
                open.push_back(idx);
                opened = true;
                break;
            case '"':
                parse_string(tape_[idx]);
                break;
            case 't':
                if (text_.compare(pos_, 4, "true") != 0) fail("bad literal");
                tape_[idx].type = Type::True;  pos_ += 4; break;
            case 'f':
                if (text_.compare(pos_, 5, "false") != 0) fail("bad literal");
                tape_[idx].type = Type::False; pos_ += 5; break;
            case 'n':
                if (text_.compare(pos_, 4, "null") != 0) fail("bad literal");
                tape_[idx].type = Type::Null;  pos_ += 4; break;
            default: {
                if (c != '-' && (c < '0' || c > '9')) fail("unexpected character");
                const std::size_t s = pos_;
                while (pos_ < end && (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
                       text_[pos_] == '-' || text_[pos_] == '+' || text_[pos_] == '.' ||
                       text_[pos_] == 'e' || text_[pos_] == 'E'))
                    ++pos_;
                tape_[idx].type = Type::Number;
                tape_[idx].a = static_cast<std::uint32_t>(s);
                tape_[idx].b = static_cast<std::uint32_t>(pos_ - s);
            }
        }
        if (!opened) tape_[idx].next = idx + 1;

        // After a value (or an opening bracket): close containers, read
        // separators and object keys until the next value starts.
        for (;;) {
            if (open.empty()) return;
            const std::uint32_t top = open.back();
            const bool is_obj = tape_[top].type == Type::Object;
            skip_ws();
            if (pos_ >= end) fail("unexpected end of input");

            const bool fresh = opened && top == idx;
            opened = false;
            const char d = text_[pos_];

            if (d == (is_obj ? '}' : ']')) {
                ++pos_;
                tape_[top].next = static_cast<std::uint32_t>(tape_.size());
                open.pop_back();
                continue;
            }
            if (!fresh) {
                if (d != ',') fail("expected ',' or closing bracket");
                ++pos_;
                skip_ws();
            }
            ++tape_[top].a;
            if (is_obj) {
                if (pos_ >= end || text_[pos_] != '"') fail("expected object key");
                const auto k = static_cast<std::uint32_t>(tape_.size());
                tape_.emplace_back();
                parse_string(tape_[k]);
                tape_[k].next = k + 1;
                skip_ws();
                if (pos_ >= end || text_[pos_] != ':') fail("expected ':'");
                ++pos_;
            }
            break;   // parse the element value
        }
    }
}

} // namespace json
