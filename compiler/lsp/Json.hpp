// compiler/lsp/Json.hpp
// Minimal JSON for the Vayu language server.  Header-only, no deps.
#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vayu::lsp {

    enum class JsonKind { Null, Bool, Int, Double, Str, Arr, Obj };

    struct JsonValue;
    using JsonPtr = std::shared_ptr<JsonValue>;

    struct JsonValue {
        JsonKind kind = JsonKind::Null;
        bool        b = false;
        long long   i = 0;
        double      d = 0.0;
        std::string s;
        std::vector<JsonPtr>                         arr;
        std::vector<std::pair<std::string, JsonPtr>> obj;

        bool isNull()   const { return kind == JsonKind::Null; }
        bool isBool()   const { return kind == JsonKind::Bool; }
        bool isInt()    const { return kind == JsonKind::Int; }
        bool isDouble() const { return kind == JsonKind::Double; }
        bool isString() const { return kind == JsonKind::Str; }
        bool isArray()  const { return kind == JsonKind::Arr; }
        bool isObject() const { return kind == JsonKind::Obj; }

        bool has(const std::string& k) const {
            for (auto& kv : obj) if (kv.first == k) return true;
            return false;
        }
        JsonPtr get(const std::string& k) const {
            for (auto& kv : obj) if (kv.first == k) return kv.second;
            return nullptr;
        }
        long long asInt() const { return i; }
        bool asBool() const { return b; }
        double asDouble() const {
            if (kind == JsonKind::Int) return (double)i;
            return d;
        }
        const std::string& asString() const { return s; }
    };

    inline JsonPtr jsonNull() { return std::make_shared<JsonValue>(); }
    inline JsonPtr jsonBool(bool v) {
        auto j = std::make_shared<JsonValue>();
        j->kind = JsonKind::Bool; j->b = v; return j;
    }
    inline JsonPtr jsonInt(long long v) {
        auto j = std::make_shared<JsonValue>();
        j->kind = JsonKind::Int; j->i = v; return j;
    }
    inline JsonPtr jsonDouble(double v) {
        auto j = std::make_shared<JsonValue>();
        j->kind = JsonKind::Double; j->d = v; return j;
    }
    inline JsonPtr jsonString(const std::string& v) {
        auto j = std::make_shared<JsonValue>();
        j->kind = JsonKind::Str; j->s = v; return j;
    }
    inline JsonPtr jsonArray() {
        auto j = std::make_shared<JsonValue>();
        j->kind = JsonKind::Arr; return j;
    }
    inline JsonPtr jsonObject() {
        auto j = std::make_shared<JsonValue>();
        j->kind = JsonKind::Obj; return j;
    }
    inline void jsonSet(const JsonPtr& o, const std::string& k, JsonPtr v) {
        o->obj.emplace_back(k, std::move(v));
    }
    inline void jsonPush(const JsonPtr& a, JsonPtr v) {
        a->arr.push_back(std::move(v));
    }

    // ------------------------------------------------------------------
    // Parser
    // ------------------------------------------------------------------
    namespace json_detail {

        inline void skipWs(const std::string& s, size_t& i) {
            while (i < s.size() &&
                (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                ++i;
        }

        inline std::string parseString(const std::string& s, size_t& i) {
            if (i >= s.size() || s[i] != '"')
                throw std::runtime_error("json: expected '\"'");
            ++i;
            std::string out;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char e = s[i + 1];
                    i += 2;
                    switch (e) {
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'u': {
                        if (i + 4 > s.size()) { out += '?'; break; }
                        unsigned int cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        }
                        if (cp < 0x80) {
                            out += (char)cp;
                        }
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += e;
                    }
                }
                else {
                    out += s[i++];
                }
            }
            if (i >= s.size()) throw std::runtime_error("json: unterminated string");
            ++i;  // closing quote
            return out;
        }

        JsonPtr parseValue(const std::string& s, size_t& i);

        inline JsonPtr parseNumber(const std::string& s, size_t& i) {
            size_t start = i;
            bool isFloat = false;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (i < s.size() && s[i] == '.') {
                isFloat = true; ++i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                isFloat = true; ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            }
            std::string num = s.substr(start, i - start);
            auto v = std::make_shared<JsonValue>();
            if (isFloat) {
                v->kind = JsonKind::Double;
                v->d = std::stod(num);
            }
            else {
                v->kind = JsonKind::Int;
                v->i = std::stoll(num);
            }
            return v;
        }

        inline JsonPtr parseArray(const std::string& s, size_t& i) {
            ++i;  // '['
            auto a = jsonArray();
            skipWs(s, i);
            if (i < s.size() && s[i] == ']') { ++i; return a; }
            for (;;) {
                skipWs(s, i);
                a->arr.push_back(parseValue(s, i));
                skipWs(s, i);
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                throw std::runtime_error("json: expected ',' or ']'");
            }
            return a;
        }

        inline JsonPtr parseObject(const std::string& s, size_t& i) {
            ++i;  // '{'
            auto o = jsonObject();
            skipWs(s, i);
            if (i < s.size() && s[i] == '}') { ++i; return o; }
            for (;;) {
                skipWs(s, i);
                std::string key = parseString(s, i);
                skipWs(s, i);
                if (i >= s.size() || s[i] != ':')
                    throw std::runtime_error("json: expected ':'");
                ++i;
                skipWs(s, i);
                o->obj.emplace_back(std::move(key), parseValue(s, i));
                skipWs(s, i);
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                throw std::runtime_error("json: expected ',' or '}'");
            }
            return o;
        }

        inline JsonPtr parseValue(const std::string& s, size_t& i) {
            skipWs(s, i);
            if (i >= s.size()) throw std::runtime_error("json: unexpected eof");
            char c = s[i];
            if (c == '{') return parseObject(s, i);
            if (c == '[') return parseArray(s, i);
            if (c == '"') return jsonString(parseString(s, i));
            if (c == 't') {
                if (s.compare(i, 4, "true") == 0) { i += 4; return jsonBool(true); }
                throw std::runtime_error("json: bad literal");
            }
            if (c == 'f') {
                if (s.compare(i, 5, "false") == 0) { i += 5; return jsonBool(false); }
                throw std::runtime_error("json: bad literal");
            }
            if (c == 'n') {
                if (s.compare(i, 4, "null") == 0) { i += 4; return jsonNull(); }
                throw std::runtime_error("json: bad literal");
            }
            if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(s, i);
            throw std::runtime_error("json: unexpected char");
        }

        inline void escapeInto(std::string& out, const std::string& s) {
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
                    }
                    else {
                        out += (char)c;
                    }
                }
            }
            out += '"';
        }

        inline void serialize(const JsonPtr& v, std::string& out) {
            if (!v || v->kind == JsonKind::Null) { out += "null"; return; }
            switch (v->kind) {
            case JsonKind::Bool:   out += v->b ? "true" : "false"; return;
            case JsonKind::Int:    out += std::to_string(v->i);    return;
            case JsonKind::Double: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", v->d);
                out += buf;
                return;
            }
            case JsonKind::Str:    escapeInto(out, v->s); return;
            case JsonKind::Arr: {
                out += '[';
                for (size_t i = 0; i < v->arr.size(); ++i) {
                    if (i) out += ',';
                    serialize(v->arr[i], out);
                }
                out += ']';
                return;
            }
            case JsonKind::Obj: {
                out += '{';
                for (size_t i = 0; i < v->obj.size(); ++i) {
                    if (i) out += ',';
                    escapeInto(out, v->obj[i].first);
                    out += ':';
                    serialize(v->obj[i].second, out);
                }
                out += '}';
                return;
            }
            default: out += "null"; return;
            }
        }

    } // namespace json_detail

    inline JsonPtr parseJson(const std::string& text) {
        size_t i = 0;
        auto v = json_detail::parseValue(text, i);
        return v;
    }

    inline std::string serializeJson(const JsonPtr& v) {
        std::string out;
        json_detail::serialize(v, out);
        return out;
    }

} // namespace vayu::lsp