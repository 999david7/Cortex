#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace js {

static const Value kNull;

size_t Value::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

bool Value::contains(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    for (const auto& kv : obj_)
        if (kv.first == key) return kv.second;
    return kNull;
}

const Value& Value::operator[](size_t index) const {
    if (type_ != Type::Array || index >= arr_.size()) return kNull;
    return arr_[index];
}

void Value::set(const std::string& key, Value v) {
    if (type_ != Type::Object) { type_ = Type::Object; arr_.clear(); }
    for (auto& kv : obj_) {
        if (kv.first == key) { kv.second = std::move(v); return; }
    }
    obj_.emplace_back(key, std::move(v));
}

void Value::push(Value v) {
    if (type_ != Type::Array) { type_ = Type::Array; obj_.clear(); }
    arr_.push_back(std::move(v));
}

// ---------------------------------------------------------------- writing

static void encode_utf8(unsigned cp, std::string& out) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c; // pass UTF-8 bytes through untouched
                }
        }
    }
    out += '"';
    return out;
}

static std::string number_to_string(double n) {
    if (std::isnan(n) || std::isinf(n)) return "0";
    if (n == (double)(long long)n && std::fabs(n) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", (long long)n);
        return buf;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", n);
    return buf;
}

void Value::dump_to(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    const std::string pad = pretty ? std::string((size_t)(indent * (depth + 1)), ' ') : std::string();
    const std::string pad_end = pretty ? std::string((size_t)(indent * depth), ' ') : std::string();

    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: out += number_to_string(num_); break;
        case Type::String: out += quote(str_); break;
        case Type::Array:
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += pad; }
                arr_[i].dump_to(out, indent, depth + 1);
            }
            if (pretty) { out += '\n'; out += pad_end; }
            out += ']';
            break;
        case Type::Object:
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += pad; }
                out += quote(obj_[i].first);
                out += ':';
                if (pretty) out += ' ';
                obj_[i].second.dump_to(out, indent, depth + 1);
            }
            if (pretty) { out += '\n'; out += pad_end; }
            out += '}';
            break;
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------- parsing

namespace {

class Parser {
public:
    Parser(const std::string& text) : s_(text) {}

    bool parse_value(Value& out) {
        skip_ws();
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        char c = s_[pos_];
        switch (c) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't':
                if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; out = Value(true); return true; }
                return fail("invalid literal");
            case 'f':
                if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; out = Value(false); return true; }
                return fail("invalid literal");
            case 'n':
                if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; out = Value(); return true; }
                return fail("invalid literal");
            default:
                return parse_number(out);
        }
    }

    void skip_ws() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool at_end() { skip_ws(); return pos_ >= s_.size(); }

    std::string error;

private:
    bool fail(const char* msg) {
        if (error.empty()) error = std::string(msg) + " at offset " + std::to_string(pos_);
        return false;
    }

    bool parse_object(Value& out) {
        out = Value::object();
        ++pos_; // '{'
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            Value v;
            if (!parse_value(v)) return false;
            out.set(key, std::move(v));
            skip_ws();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Value& out) {
        out = Value::array();
        ++pos_; // '['
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.push(std::move(v));
            skip_ws();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_string(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected string");
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
                        s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            encode_utf8(lo, out); // lone surrogate: emit as-is
                    }
                    encode_utf8(cp, out);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) return fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool parse_number(Value& out) {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' ||
                                    s_[pos_] == 'e' || s_[pos_] == 'E' ||
                                    ((s_[pos_] == '-' || s_[pos_] == '+') &&
                                     (s_[pos_ - 1] == 'e' || s_[pos_ - 1] == 'E')))) {
            any = true;
            ++pos_;
        }
        if (!any) return fail("invalid value");
        out = Value(std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr));
        return true;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

} // namespace

Value parse(const std::string& text, std::string* err) {
    Parser p(text);
    Value v;
    if (!p.parse_value(v)) {
        if (err) *err = p.error;
        return Value();
    }
    if (err) err->clear();
    return v;
}

} // namespace js
