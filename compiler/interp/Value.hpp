#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vayu {

    class  Environment;
    struct DefStmt;
    struct LambdaExpr;
    struct Chunk;
    class  Value;
    struct ClassObject;
    struct ListValue;
    struct MapValue;
    struct ModuleValue;
    struct StructInstance;
    struct Callable;
    struct GeneratorValue;
    struct TupleValue;
    struct SetValue;
    struct GeneratorCancelled {};

    class Value {
    public:
        using FnPtr = std::shared_ptr<Callable>;
        using InstPtr = std::shared_ptr<StructInstance>;
        using ClassPtr = std::shared_ptr<ClassObject>;
        using ListPtr = std::shared_ptr<ListValue>;
        using MapPtr = std::shared_ptr<MapValue>;
        using ModulePtr = std::shared_ptr<ModuleValue>;
        using GeneratorPtr = std::shared_ptr<GeneratorValue>;
        using TuplePtr = std::shared_ptr<TupleValue>;
        using SetPtr = std::shared_ptr<SetValue>;

    private:
        enum class Tag : uint8_t {
            None, Bool, Int, Float,
            Str, Callable, Instance, Class,
            List, Map, Module, Generator,
            Tuple, Set,
        };

        union U {
            bool        b;
            long long   i;
            double      f;
            std::string s;
            FnPtr       callable;
            InstPtr     inst;
            ClassPtr    cls;
            ListPtr     list;
            MapPtr      map;
            ModulePtr   module;
            GeneratorPtr generator;
            TuplePtr    tuple;
            SetPtr      set;

            U() noexcept : i(0) {}
            ~U() {}
        };

        Tag tag_;
        U   u_;

        void destroy()  noexcept;
        void copyFrom(const Value& other);
        void moveFrom(Value&& other) noexcept;

    public:
        Value() noexcept : tag_(Tag::None) {}
        Value(std::nullptr_t) noexcept : tag_(Tag::None) {}
        Value(bool b) noexcept : tag_(Tag::Bool) { u_.b = b; }
        Value(long long i) noexcept : tag_(Tag::Int) { u_.i = i; }
        Value(int i) noexcept : tag_(Tag::Int) { u_.i = (long long)i; }
        Value(double f) noexcept : tag_(Tag::Float) { u_.f = f; }
        Value(std::string s);
        Value(const char* s);
        Value(FnPtr c)        noexcept;
        Value(InstPtr s)      noexcept;
        Value(ClassPtr c)     noexcept;
        Value(ListPtr l)      noexcept;
        Value(MapPtr m)       noexcept;
        Value(ModulePtr m)    noexcept;
        Value(GeneratorPtr g) noexcept;
        Value(TuplePtr t)     noexcept;
        Value(SetPtr s)       noexcept;

        Value(const Value& other);
        Value(Value&& other) noexcept;
        ~Value();
        Value& operator=(const Value& other);
        Value& operator=(Value&& other) noexcept;

        bool isNone()      const noexcept { return tag_ == Tag::None; }
        bool isBool()      const noexcept { return tag_ == Tag::Bool; }
        bool isInt()       const noexcept { return tag_ == Tag::Int; }
        bool isFloat()     const noexcept { return tag_ == Tag::Float; }
        bool isNumber()    const noexcept { return tag_ == Tag::Int || tag_ == Tag::Float; }
        bool isString()    const noexcept { return tag_ == Tag::Str; }
        bool isCallable()  const noexcept { return tag_ == Tag::Callable; }
        bool isInstance()  const noexcept { return tag_ == Tag::Instance; }
        bool isClass()     const noexcept { return tag_ == Tag::Class; }
        bool isList()      const noexcept { return tag_ == Tag::List; }
        bool isMap()       const noexcept { return tag_ == Tag::Map; }
        bool isModule()    const noexcept { return tag_ == Tag::Module; }
        bool isGenerator() const noexcept { return tag_ == Tag::Generator; }
        bool isTuple()     const noexcept { return tag_ == Tag::Tuple; }
        bool isSet()       const noexcept { return tag_ == Tag::Set; }

        bool               asBool()    const { return u_.b; }
        long long          asInt()     const { return u_.i; }
        double             asFloat()   const { return u_.f; }
        const std::string& asString()  const { return u_.s; }
        FnPtr        asCallable()  const { return u_.callable; }
        InstPtr      asInstance()  const { return u_.inst; }
        ClassPtr     asClass()     const { return u_.cls; }
        ListPtr      asList()      const { return u_.list; }
        MapPtr       asMap()       const { return u_.map; }
        ModulePtr    asModule()    const { return u_.module; }
        GeneratorPtr asGenerator() const { return u_.generator; }
        TuplePtr     asTuple()     const { return u_.tuple; }
        SetPtr       asSet()       const { return u_.set; }

        double asDouble() const noexcept {
            return tag_ == Tag::Int ? (double)u_.i : u_.f;
        }

        bool        truthy()   const;
        std::string toString() const;
        std::string typeName() const;
    };

    // ===========================================================================
    // Runtime object types
    // ===========================================================================

    struct StructInstance {
        std::shared_ptr<ClassObject>           cls;
        std::unordered_map<std::string, Value> fields;
    };

    struct ListValue {
        std::vector<Value> items;
    };

    struct MapValue {
        std::unordered_map<std::string, Value> entries;
    };

    struct ModuleValue {
        std::string                            name;
        std::unordered_map<std::string, Value> members;
    };

    struct ClassObject {
        std::string                            name;
        std::vector<std::string>               fieldOrder;
        std::shared_ptr<ClassObject>           parent;
        std::unordered_map<std::string, Value> staticFields;
    };

    // Phase 14.0: immutable, fixed-length, tagged.
    struct TupleValue {
        std::vector<Value> items;
    };

    // Phase 14.1: order-preserving, dedup-on-insert.
    struct SetValue {
        std::vector<Value> items;
    };

    // ===========================================================================
    // Callable
    // ===========================================================================

    using NativeFnPtr = Value(*)(const std::vector<Value>&);

    struct Callable {
        enum class Kind {
            Native,
            User,
            ClassCtor,
            BoundMethod,
            SuperMethod,
            ListMethod,
            MapMethod,
            StringMethod,
            Lambda,
            VMFunction,
            TupleMethod,
            SetMethod,
        } kind = Kind::Native;

        std::string name;

        NativeFnPtr nativeFn = nullptr;

        const DefStmt* decl = nullptr;
        std::shared_ptr<Environment> closure;
        std::shared_ptr<ClassObject> definingClass;
        const LambdaExpr* lambdaExpr = nullptr;

        std::shared_ptr<Chunk>       chunk;
        std::vector<std::string>     vmParams;
        bool                         isGenerator = false;

        std::shared_ptr<ClassObject> classObj;

        std::shared_ptr<StructInstance> boundSelf;
        std::shared_ptr<Callable>       methodFn;
        std::shared_ptr<ClassObject>    superParent;

        std::shared_ptr<ListValue> boundList;
        std::shared_ptr<MapValue>  boundMap;
        std::string                boundStr;
        std::shared_ptr<TupleValue> boundTuple;
        std::shared_ptr<SetValue>   boundSet;
    };

    // ===========================================================================
    // Generator
    // ===========================================================================

    enum class GenState { Fresh, Running, Suspended, Done };

    struct GeneratorValue {
        std::thread             worker;
        std::mutex              mtx;
        std::condition_variable cv;
        GenState                state = GenState::Fresh;
        bool                    resume = false;
        bool                    cancel = false;
        Value                   yielded;
        std::exception_ptr      pendingError;

        std::shared_ptr<Environment>    ownerEnv;
        std::shared_ptr<GeneratorValue> ownerGen;

        ~GeneratorValue();
    };

} // namespace vayu