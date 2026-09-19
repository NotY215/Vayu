#include "Value.hpp"
#include <cmath>
#include <sstream>

namespace vayu {

    void Value::destroy() noexcept {
        if (static_cast<uint8_t>(tag_) < static_cast<uint8_t>(Tag::Str)) return;
        switch (tag_) {
        case Tag::Str:       u_.s.~basic_string();        break;
        case Tag::Callable:  u_.callable.~shared_ptr();   break;
        case Tag::Instance:  u_.inst.~shared_ptr();       break;
        case Tag::Class:     u_.cls.~shared_ptr();        break;
        case Tag::List:      u_.list.~shared_ptr();       break;
        case Tag::Map:       u_.map.~shared_ptr();        break;
        case Tag::Module:    u_.module.~shared_ptr();     break;
        case Tag::Generator: u_.generator.~shared_ptr();  break;
        case Tag::Tuple:     u_.tuple.~shared_ptr();      break;
        case Tag::Set:       u_.set.~shared_ptr();        break;
        case Tag::Ref:       u_.ref.~shared_ptr();        break;
        default: break;
        }
    }

    void Value::copyFrom(const Value& other) {
        tag_ = other.tag_;
        switch (tag_) {
        case Tag::None:   break;
        case Tag::Bool:   u_.b = other.u_.b; break;
        case Tag::Int:    u_.i = other.u_.i; break;
        case Tag::Float:  u_.f = other.u_.f; break;
        case Tag::Str:       new (&u_.s) std::string(other.u_.s);            break;
        case Tag::Callable:  new (&u_.callable) FnPtr(other.u_.callable);    break;
        case Tag::Instance:  new (&u_.inst) InstPtr(other.u_.inst);          break;
        case Tag::Class:     new (&u_.cls) ClassPtr(other.u_.cls);           break;
        case Tag::List:      new (&u_.list) ListPtr(other.u_.list);          break;
        case Tag::Map:       new (&u_.map) MapPtr(other.u_.map);             break;
        case Tag::Module:    new (&u_.module) ModulePtr(other.u_.module);    break;
        case Tag::Generator: new (&u_.generator) GeneratorPtr(other.u_.generator); break;
        case Tag::Tuple:     new (&u_.tuple) TuplePtr(other.u_.tuple);       break;
        case Tag::Set:       new (&u_.set) SetPtr(other.u_.set);             break;
        case Tag::Ref:       new (&u_.ref) RefPtr(other.u_.ref);             break;
        }
    }

    void Value::moveFrom(Value&& other) noexcept {
        tag_ = other.tag_;
        switch (tag_) {
        case Tag::None:   break;
        case Tag::Bool:   u_.b = other.u_.b; break;
        case Tag::Int:    u_.i = other.u_.i; break;
        case Tag::Float:  u_.f = other.u_.f; break;
        case Tag::Str:       new (&u_.s) std::string(std::move(other.u_.s));             break;
        case Tag::Callable:  new (&u_.callable) FnPtr(std::move(other.u_.callable));     break;
        case Tag::Instance:  new (&u_.inst) InstPtr(std::move(other.u_.inst));           break;
        case Tag::Class:     new (&u_.cls) ClassPtr(std::move(other.u_.cls));            break;
        case Tag::List:      new (&u_.list) ListPtr(std::move(other.u_.list));           break;
        case Tag::Map:       new (&u_.map) MapPtr(std::move(other.u_.map));              break;
        case Tag::Module:    new (&u_.module) ModulePtr(std::move(other.u_.module));     break;
        case Tag::Generator: new (&u_.generator) GeneratorPtr(std::move(other.u_.generator)); break;
        case Tag::Tuple:     new (&u_.tuple) TuplePtr(std::move(other.u_.tuple));        break;
        case Tag::Set:       new (&u_.set) SetPtr(std::move(other.u_.set));              break;
        case Tag::Ref:       new (&u_.ref) RefPtr(std::move(other.u_.ref));              break;
        }
    }

    Value::Value(std::string s) : tag_(Tag::Str) {
        new (&u_.s) std::string(std::move(s));
    }

    Value::Value(const char* s) : tag_(Tag::Str) {
        new (&u_.s) std::string(s);
    }

    Value::Value(FnPtr c)        noexcept : tag_(Tag::Callable) { new (&u_.callable)  FnPtr(std::move(c)); }
    Value::Value(InstPtr s)      noexcept : tag_(Tag::Instance) { new (&u_.inst)      InstPtr(std::move(s)); }
    Value::Value(ClassPtr c)     noexcept : tag_(Tag::Class) { new (&u_.cls)       ClassPtr(std::move(c)); }
    Value::Value(ListPtr l)      noexcept : tag_(Tag::List) { new (&u_.list)      ListPtr(std::move(l)); }
    Value::Value(MapPtr m)       noexcept : tag_(Tag::Map) { new (&u_.map)       MapPtr(std::move(m)); }
    Value::Value(ModulePtr m)    noexcept : tag_(Tag::Module) { new (&u_.module)    ModulePtr(std::move(m)); }
    Value::Value(GeneratorPtr g) noexcept : tag_(Tag::Generator) { new (&u_.generator) GeneratorPtr(std::move(g)); }
    Value::Value(TuplePtr t)     noexcept : tag_(Tag::Tuple) { new (&u_.tuple)     TuplePtr(std::move(t)); }
    Value::Value(SetPtr s)       noexcept : tag_(Tag::Set) { new (&u_.set)       SetPtr(std::move(s)); }
    Value::Value(RefPtr r)       noexcept : tag_(Tag::Ref) { new (&u_.ref)       RefPtr(std::move(r)); }

    Value::Value(const Value& other) { copyFrom(other); }
    Value::Value(Value&& other) noexcept { moveFrom(std::move(other)); }
    Value::~Value() { destroy(); }

    Value& Value::operator=(const Value& other) {
        if (this != &other) { destroy(); copyFrom(other); }
        return *this;
    }
    Value& Value::operator=(Value&& other) noexcept {
        if (this != &other) { destroy(); moveFrom(std::move(other)); }
        return *this;
    }

    static std::string formatDouble(double d) {
        if (std::isnan(d)) return "nan";
        if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
        std::ostringstream oss; oss.precision(15); oss << d;
        std::string s = oss.str();
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
        return s;
    }

    bool Value::truthy() const {
        switch (tag_) {
        case Tag::None:  return false;
        case Tag::Bool:  return u_.b;
        case Tag::Int:   return u_.i != 0;
        case Tag::Float: return u_.f != 0.0;
        case Tag::Str:   return !u_.s.empty();
        case Tag::List:  return !u_.list->items.empty();
        case Tag::Map:   return !u_.map->entries.empty();
        case Tag::Tuple: return !u_.tuple->items.empty();
        case Tag::Set:   return !u_.set->items.empty();
        case Tag::Ref:   return true;
        default:         return true;
        }
    }

    static void emitInner(std::string& out, const Value& v, bool inContainer);

    std::string Value::toString() const {
        std::string out;
        emitInner(out, *this, false);
        return out;
    }

    static void emitInner(std::string& out, const Value& v, bool /*inContainer*/) {
        switch (v.isNone() ? 100 :
            v.isBool() ? 101 :
            v.isInt() ? 102 :
            v.isFloat() ? 103 :
            v.isString() ? 104 :
            v.isCallable() ? 105 :
            v.isClass() ? 106 :
            v.isModule() ? 107 :
            v.isGenerator() ? 108 :
            v.isList() ? 109 :
            v.isMap() ? 110 :
            v.isInstance() ? 111 :
            v.isTuple() ? 112 :
            v.isSet() ? 113 :
            v.isRef() ? 114 : -1) {
        case 100: out += "None"; return;
        case 101: out += v.asBool() ? "true" : "false"; return;
        case 102: out += std::to_string(v.asInt()); return;
        case 103: out += formatDouble(v.asFloat()); return;
        case 104: out += v.asString(); return;
        case 105: out += "<function " + v.asCallable()->name + ">"; return;
        case 106: out += "<class " + v.asClass()->name + ">"; return;
        case 107: out += "<module " + v.asModule()->name + ">"; return;
        case 108: out += "<generator>"; return;
        case 114: out += "<ref>"; return;
        case 109: {
            out += "[";
            const auto& items = v.asList()->items;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out += ", ";
                if (items[i].isString()) out += "\"" + items[i].asString() + "\"";
                else { std::string s; emitInner(s, items[i], true); out += s; }
            }
            out += "]"; return;
        }
        case 110: {
            out += "{";
            bool first = true;
            for (auto& [k, vv] : v.asMap()->entries) {
                if (!first) out += ", ";
                first = false;
                out += "\"" + k + "\": ";
                if (vv.isString()) out += "\"" + vv.asString() + "\"";
                else { std::string s; emitInner(s, vv, true); out += s; }
            }
            out += "}"; return;
        }
        case 111: {
            auto s = v.asInstance();
            out += s->cls ? s->cls->name : "?";
            out += "(";
            bool first = true;
            auto emit = [&](const std::string& fn, const Value& vv) {
                if (!first) out += ", ";
                first = false;
                out += fn + "=";
                if (vv.isString()) out += "\"" + vv.asString() + "\"";
                else { std::string ss; emitInner(ss, vv, true); out += ss; }
                };
            if (s->cls) {
                for (const auto& fn : s->cls->fieldOrder) {
                    auto it = s->fields.find(fn);
                    if (it != s->fields.end()) emit(fn, it->second);
                }
            }
            for (auto& [fn, vv] : s->fields) {
                if (s->cls) {
                    bool declared = false;
                    for (auto& d : s->cls->fieldOrder) if (d == fn) { declared = true; break; }
                    if (declared) continue;
                }
                emit(fn, vv);
            }
            out += ")"; return;
        }
        case 112: {
            const auto& items = v.asTuple()->items;
            out += "(";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out += ", ";
                if (items[i].isString()) out += "\"" + items[i].asString() + "\"";
                else { std::string s; emitInner(s, items[i], true); out += s; }
            }
            if (items.size() == 1) out += ",";
            out += ")"; return;
        }
        case 113: {
            const auto& items = v.asSet()->items;
            if (items.empty()) { out += "set()"; return; }
            out += "{";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out += ", ";
                if (items[i].isString()) out += "\"" + items[i].asString() + "\"";
                else { std::string s; emitInner(s, items[i], true); out += s; }
            }
            out += "}"; return;
        }
        default: out += "<unknown>"; return;
        }
    }

    std::string Value::typeName() const {
        switch (tag_) {
        case Tag::None:      return "None";
        case Tag::Bool:      return "bool";
        case Tag::Int:       return "int";
        case Tag::Float:     return "float";
        case Tag::Str:       return "str";
        case Tag::Callable:  return "function";
        case Tag::Instance:  return u_.inst->cls ? u_.inst->cls->name : "?";
        case Tag::Class:     return u_.cls->name;
        case Tag::List:      return "list";
        case Tag::Map:       return "map";
        case Tag::Module:    return "module";
        case Tag::Generator: return "generator";
        case Tag::Tuple:     return "tuple";
        case Tag::Set:       return "set";
        case Tag::Ref:       return "ref";
        }
        return "?";
    }

    GeneratorValue::~GeneratorValue() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (state != GenState::Done) {
                cancel = true;
                cv.notify_all();
            }
        }
        if (worker.joinable()) worker.join();
    }

} // namespace vayu