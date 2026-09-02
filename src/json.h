// Minimal JSON parser / writer. No dependencies.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace js {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Value(double n) : type_(Type::Number), num_(n) {}
    explicit Value(int n) : type_(Type::Number), num_((double)n) {}
    Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Value object() { Value v; v.type_ = Type::Object; return v; }
    static Value array() { Value v; v.type_ = Type::Array; return v; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Lenient accessors: return the fallback when the type does not match.
    std::string as_string(const std::string& def = "") const { return type_ == Type::String ? str_ : def; }
    double as_double(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    int as_int(int def = 0) const { return type_ == Type::Number ? (int)num_ : def; }
    bool as_bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }

    size_t size() const;
    bool contains(const std::string& key) const;

    // Missing keys / out-of-range indices yield a shared null value.
    const Value& operator[](const std::string& key) const;
    const Value& operator[](size_t index) const;

    const std::vector<Value>& items() const { return arr_; }
    const std::vector<std::pair<std::string, Value>>& fields() const { return obj_; }

    void set(const std::string& key, Value v);
    void push(Value v);

    std::string dump(int indent = -1) const;

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

// Returns a Null value on failure; err (when given) receives the reason.
Value parse(const std::string& text, std::string* err = nullptr);

// Escapes a raw string into a quoted JSON string literal.
std::string quote(const std::string& s);

} // namespace js
