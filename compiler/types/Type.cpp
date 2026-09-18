#include "Type.hpp"

namespace vayu {

    const StructFieldInfo* Type::findField(const std::string& n) const {
        for (auto& f : fields) if (f.name == n) return &f;
        if (parent) return parent->findField(n);
        return nullptr;
    }
    TypePtr Type::findMethod(const std::string& n) const {
        auto it = methods.find(n);
        if (it != methods.end()) return it->second;
        if (parent) return parent->findMethod(n);
        return nullptr;
    }

    std::string Type::toString() const {
        switch (kind) {
        case TypeKind::None: return "None"; case TypeKind::Bool: return "bool";
        case TypeKind::Int: return "int"; case TypeKind::Float: return "float";
        case TypeKind::Str: return "str"; case TypeKind::Char: return "char";
        case TypeKind::Bytes: return "bytes"; case TypeKind::Any: return "any";
        case TypeKind::Unknown: return "?"; case TypeKind::Error: return "<error>";
        case TypeKind::Named: return name;
        case TypeKind::Struct: return name;
        case TypeKind::TypeParam: {
            auto pos = name.find('#');
            return pos == std::string::npos ? name : name.substr(0, pos);
        }
        case TypeKind::Unique: {
            std::string s = "unique<";
            s += params.empty() ? "?" : params[0]->toString();
            s += ">";
            return s;
        }
        case TypeKind::Shared: {
            std::string s = "shared<";
            s += params.empty() ? "?" : params[0]->toString();
            s += ">";
            return s;
        }
        case TypeKind::Weak: {
            std::string s = "weak<";
            s += params.empty() ? "?" : params[0]->toString();
            s += ">";
            return s;
        }
        case TypeKind::List: {
            std::string s = "list<";
            if (!params.empty()) s += params[0]->toString();
            else s += "?";
            s += ">";
            return s;
        }
        case TypeKind::Map: {
            std::string s = "map<";
            s += params.size() > 0 ? params[0]->toString() : "?";
            s += ", ";
            s += params.size() > 1 ? params[1]->toString() : "?";
            s += ">";
            return s;
        }
        case TypeKind::Function: {
            std::string s = "(";
            for (size_t i = 0; i < params.size(); ++i) {
                if (i) s += ", ";
                s += params[i]->toString();
            }
            s += ") -> ";
            s += returnType ? returnType->toString() : "?";
            return s;
        }
        }
        return "?";
    }

    bool Type::equals(const TypePtr& other) const {
        if (!other) return false;
        if (kind != other->kind) return false;
        if (kind == TypeKind::TypeParam)
            return name == other->name;
        if (kind == TypeKind::Named || kind == TypeKind::Struct)
            return name == other->name;
        if (kind == TypeKind::List || kind == TypeKind::Map ||
            kind == TypeKind::Unique || kind == TypeKind::Shared ||
            kind == TypeKind::Weak) {
            if (params.size() != other->params.size()) return false;
            for (size_t i = 0; i < params.size(); ++i)
                if (!params[i]->equals(other->params[i])) return false;
            return true;
        }
        if (kind == TypeKind::Function) {
            if (params.size() != other->params.size()) return false;
            for (size_t i = 0; i < params.size(); ++i)
                if (!params[i]->equals(other->params[i])) return false;
            if (!returnType && !other->returnType) return true;
            if (!returnType || !other->returnType) return false;
            return returnType->equals(other->returnType);
        }
        return true;
    }

    namespace Types {
        static TypePtr make(TypeKind k) { return std::make_shared<Type>(k); }
        TypePtr None() { static auto t = make(TypeKind::None);    return t; }
        TypePtr Bool() { static auto t = make(TypeKind::Bool);    return t; }
        TypePtr Int() { static auto t = make(TypeKind::Int);     return t; }
        TypePtr Float() { static auto t = make(TypeKind::Float);   return t; }
        TypePtr Str() { static auto t = make(TypeKind::Str);     return t; }
        TypePtr Char() { static auto t = make(TypeKind::Char);    return t; }
        TypePtr Bytes() { static auto t = make(TypeKind::Bytes);   return t; }
        TypePtr Any() { static auto t = make(TypeKind::Any);     return t; }
        TypePtr Unknown() { static auto t = make(TypeKind::Unknown); return t; }
        TypePtr Error() { static auto t = make(TypeKind::Error);   return t; }

        TypePtr Function(std::vector<TypePtr> params, TypePtr ret) {
            auto t = std::make_shared<Type>(TypeKind::Function);
            t->params = std::move(params); t->returnType = std::move(ret);
            return t;
        }
        TypePtr Struct(std::string name, std::vector<StructFieldInfo> fields) {
            auto t = std::make_shared<Type>(TypeKind::Struct, std::move(name));
            t->fields = std::move(fields);
            return t;
        }
        TypePtr List(TypePtr elem) {
            auto t = std::make_shared<Type>(TypeKind::List);
            t->params.push_back(std::move(elem));
            return t;
        }
        TypePtr Map(TypePtr key, TypePtr value) {
            auto t = std::make_shared<Type>(TypeKind::Map);
            t->params.push_back(std::move(key));
            t->params.push_back(std::move(value));
            return t;
        }
        TypePtr Unique(TypePtr elem) {
            auto t = std::make_shared<Type>(TypeKind::Unique);
            t->params.push_back(std::move(elem));
            return t;
        }
        TypePtr Shared(TypePtr elem) {
            auto t = std::make_shared<Type>(TypeKind::Shared);
            t->params.push_back(std::move(elem));
            return t;
        }
        TypePtr Weak(TypePtr elem) {
            auto t = std::make_shared<Type>(TypeKind::Weak);
            t->params.push_back(std::move(elem));
            return t;
        }
    } // namespace Types

    bool isAssignable(const TypePtr& to, const TypePtr& from) {
        if (!to || !from) return false;
        if (to->kind == TypeKind::Any || from->kind == TypeKind::Any) return true;
        if (to->kind == TypeKind::Error || from->kind == TypeKind::Error) return true;
        if (to->kind == TypeKind::Unknown || from->kind == TypeKind::Unknown) return true;
        if (to->kind == TypeKind::Float && from->kind == TypeKind::Int) return true;

        if (to->kind == TypeKind::TypeParam)
            return from->kind == TypeKind::TypeParam && to->name == from->name;
        if (from->kind == TypeKind::TypeParam)
            return false;

        if (to->kind == TypeKind::Struct && from->kind == TypeKind::Struct) {
            if (to->name == from->name) return true;
            for (auto c = from->parent; c; c = c->parent)
                if (c->name == to->name) return true;
            return false;
        }
        if (to->kind == TypeKind::Named && from->kind == TypeKind::Named)
            return to->name == from->name;

        // Phase 12.0: unique<T>
        if (to->kind == TypeKind::Unique && from->kind == TypeKind::Unique) {
            if (to->params.empty() || from->params.empty()) return true;
            return to->params[0]->equals(from->params[0]);
        }

        // Phase 12.1: shared<T>
        if (to->kind == TypeKind::Shared && from->kind == TypeKind::Shared) {
            if (to->params.empty() || from->params.empty()) return true;
            return to->params[0]->equals(from->params[0]);
        }

        // Phase 12.1: weak<T>
        if (to->kind == TypeKind::Weak && from->kind == TypeKind::Weak) {
            if (to->params.empty() || from->params.empty()) return true;
            return to->params[0]->equals(from->params[0]);
        }

        // shared<T> -> weak<T> (non-owning view)
        if (to->kind == TypeKind::Weak && from->kind == TypeKind::Shared) {
            if (to->params.empty() || from->params.empty()) return true;
            return to->params[0]->equals(from->params[0]);
        }

        // weak<T> -> shared<T>.  In the current erasure model, the parser
        // desugars `w.upgrade()` to bare `w`; this rule makes the resulting
        // bare-name expression assignable to a `shared<T>` slot.
        if (to->kind == TypeKind::Shared && from->kind == TypeKind::Weak) {
            if (to->params.empty() || from->params.empty()) return true;
            return to->params[0]->equals(from->params[0]);
        }

        // Wrap sites: plain T initialises a unique<T> / shared<T> slot.
        if (to->kind == TypeKind::Unique && from->kind != TypeKind::Unique) {
            if (to->params.empty()) return true;
            return isAssignable(to->params[0], from);
        }
        if (to->kind == TypeKind::Shared && from->kind != TypeKind::Shared) {
            if (to->params.empty()) return true;
            return isAssignable(to->params[0], from);
        }

        // Nothing else crosses the ownership boundary.
        if (from->kind == TypeKind::Unique || from->kind == TypeKind::Shared ||
            from->kind == TypeKind::Weak ||
            to->kind == TypeKind::Unique || to->kind == TypeKind::Shared ||
            to->kind == TypeKind::Weak)
            return false;

        // Collections.
        if (to->kind == TypeKind::List && from->kind == TypeKind::List) {
            TypePtr a = to->params.empty() ? Types::Any() : to->params[0];
            TypePtr b = from->params.empty() ? Types::Any() : from->params[0];
            if (a->kind == TypeKind::Any || b->kind == TypeKind::Any) return true;
            return a->equals(b);
        }
        if (to->kind == TypeKind::Map && from->kind == TypeKind::Map) {
            TypePtr aK = to->params.size() > 0 ? to->params[0] : Types::Any();
            TypePtr aV = to->params.size() > 1 ? to->params[1] : Types::Any();
            TypePtr bK = from->params.size() > 0 ? from->params[0] : Types::Any();
            TypePtr bV = from->params.size() > 1 ? from->params[1] : Types::Any();
            bool keyOK = (aK->kind == TypeKind::Any || bK->kind == TypeKind::Any)
                ? true : aK->equals(bK);
            bool valOK = (aV->kind == TypeKind::Any || bV->kind == TypeKind::Any)
                ? true : aV->equals(bV);
            return keyOK && valOK;
        }

        return to->equals(from);
    }

    TypePtr commonNumeric(const TypePtr& a, const TypePtr& b) {
        if (a->kind == TypeKind::Error || b->kind == TypeKind::Error) return Types::Error();
        if (a->kind == TypeKind::Any || b->kind == TypeKind::Any) return Types::Any();
        bool aN = a->kind == TypeKind::Int || a->kind == TypeKind::Float;
        bool bN = b->kind == TypeKind::Int || b->kind == TypeKind::Float;
        if (!aN || !bN) return Types::Error();
        if (a->kind == TypeKind::Int && b->kind == TypeKind::Int) return Types::Int();
        return Types::Float();
    }

} // namespace vayu