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
        Struct,   // covers struct AND class
        List,     // params[0] = element type
        Map,      // params[0] = key type, params[1] = value type
        Named,
        TypeParam,   // Phase 11.2: `T` in a generic signature/body
    };

    class Type;
    using TypePtr = std::shared_ptr<Type>;

    struct StructFieldInfo {
        std::string name;
        TypePtr     type;
        int8_t      vis = 0;   // 0=public, 1=protected, 2=private
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

        // Phase 11.2: for a generic function signature, maps the user-visible
        // type-parameter name (`T`) to a fresh, uniquely-named TypeParam node
        // (`T#0`).  Empty for non-generic functions.
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
    }

    bool    isAssignable(const TypePtr& to, const TypePtr& from);
    TypePtr commonNumeric(const TypePtr& a, const TypePtr& b);

} // namespace vayu