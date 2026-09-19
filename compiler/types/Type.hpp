#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vayu {

    enum class TypeKind {
        None, Bool, Int, Float, Str, Char, Bytes,
        Any, Unknown, Error,
        Function,
        Struct,
        List,
        Map,
        Named,
        TypeParam,
        Unique,   // Phase 12.0 — unique<T>
        Shared,   // Phase 12.1 — shared<T>, copyable
        Weak,     // Phase 12.1 — weak<T>, must .upgrade()
        Tuple,    // Phase 14.0
        Set,      // Phase 14.1
        Ptr,      // Phase 15.2e — ptr<T>
    };

    class Type;
    using TypePtr = std::shared_ptr<Type>;

    struct StructFieldInfo {
        std::string name;
        TypePtr     type;
        int8_t      vis = 0;
    };

    class Type {
    public:
        TypeKind             kind;
        std::string          name;
        std::vector<TypePtr> params;
        TypePtr              returnType;

        std::vector<StructFieldInfo>             fields;
        std::unordered_map<std::string, TypePtr> methods;
        std::unordered_map<std::string, int8_t>  methodVis;
        TypePtr                                  parent;

        std::unordered_map<std::string, TypePtr> typeParams;
        std::unordered_map<std::string, TypePtr> typeParamConstraints;

        explicit Type(TypeKind k) : kind(k) {}
        Type(TypeKind k, std::string n) : kind(k), name(std::move(n)) {}

        std::string toString() const;
        bool        equals(const TypePtr& other) const;
        const StructFieldInfo* findField(const std::string& n) const;
        TypePtr                findMethod(const std::string& n) const;
    };

    namespace Types {
        TypePtr None(); TypePtr Bool(); TypePtr Int(); TypePtr Float();
        TypePtr Str(); TypePtr Char(); TypePtr Bytes();
        TypePtr Any(); TypePtr Unknown(); TypePtr Error();
        TypePtr Function(std::vector<TypePtr> params, TypePtr ret);
        TypePtr Struct(std::string name, std::vector<StructFieldInfo> fields);
        TypePtr List(TypePtr elem);
        TypePtr Map(TypePtr key, TypePtr value);
        TypePtr Unique(TypePtr elem);
        TypePtr Shared(TypePtr elem);
        TypePtr Weak(TypePtr elem);
        TypePtr Tuple(std::vector<TypePtr> elems);
        TypePtr Set(TypePtr elem);
        TypePtr Ptr(TypePtr elem);   // Phase 15.2e
    }

    bool    isAssignable(const TypePtr& to, const TypePtr& from);
    TypePtr commonNumeric(const TypePtr& a, const TypePtr& b);

} // namespace vayu