#pragma once
#include "Environment.hpp"
#include "Value.hpp"
#include "ast/Ast.hpp"
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <lexer/Token.hpp>
#include <functional>

namespace vayu {

    class RuntimeError : public std::runtime_error {
    public:
        SourceLocation loc;
        RuntimeError(std::string msg, SourceLocation l)
            : std::runtime_error(std::move(msg)), loc(l) {}
    };

    struct VayuException {
        Value          value;
        SourceLocation loc;
    };

    class Interpreter {
    public:
        static Interpreter* current_;

        Interpreter();
        void run(const Block& program);
        Value callValue(const Value& callee, const std::vector<Value>& args,
            SourceLocation loc);
        std::shared_ptr<Environment> globals() const { return globals_; }
        void registerDeclarations(const Block& program);
        bool  vmIsInstanceOf(const Value& v, const std::string& className);
        Value vmMakeException(const std::string& typeName, const std::string& msg);
        // Phase 11.1k1: generator support.
        Value createGenerator(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args);
        Value nextGenerator(const std::shared_ptr<GeneratorValue>& gen,
            SourceLocation loc);
        bool  tryNextGenerator(const std::shared_ptr<GeneratorValue>& gen,
            Value& out, SourceLocation loc);

        Value vmGetAttr(const Value& base, const std::string& name, SourceLocation loc);
        void  vmSetAttr(const Value& base, const std::string& name,
            const Value& v, SourceLocation loc);
        Value vmNewInst(const std::string& className,
            const std::vector<Value>& args, SourceLocation loc);
        std::shared_ptr<Callable> vmLookupClass(const std::string& name);
        Value vmLoadModule(const std::string& name, SourceLocation loc);

        using VMFunctionRunner = std::function<Value(std::shared_ptr<Callable>,
            const std::vector<Value>&)>;
        void setVMFunctionRunner(VMFunctionRunner r) { vmRunner_ = std::move(r); }

        void setSourceDir(const std::string& dir) { sourceDir_ = dir; }

        static std::string exceptionTypeName(const Value& v);
        static std::string exceptionMessage(const Value& v);

        std::unordered_map<std::string, std::shared_ptr<ClassObject>> classes_;
        std::unordered_map<std::string, const ClassStmt*>             classDecls_;

    private:
        std::shared_ptr<Environment> globals_;
        std::shared_ptr<Environment> env_;
        std::string                  sourceDir_;

        std::unordered_map<std::string, std::shared_ptr<ModuleValue>> moduleCache_;
        std::vector<std::unique_ptr<Block>> moduleAsts_;

        std::vector<Value> activeExceptions_;
        std::shared_ptr<GeneratorValue> generatorContext_;

        void  exec(const Stmt* s);
        void  execBlock(const Block& b);
        Value eval(const Expr* e);
        Value evalBinary(const BinaryExpr* b);
        Value evalAttr(const AttrExpr* a);
        Value evalCall(const CallExpr* c);
        Value evalIndex(const IndexExpr* ix);
        Value evalListLit(const ListLitExpr* n);
        Value evalMapLit(const MapLitExpr* n);
        void  execAssign(const AssignStmt* n);
        void  execFor(const ForStmt* n);
        void  execTry(const TryStmt* t);
        void  execRaise(const RaiseStmt* r);
        void  execYield(const YieldStmt* y);
        void  execImport(const ImportStmt* n);
        void  execFromImport(const FromImportStmt* n);
        VMFunctionRunner vmRunner_;

        Value loadModule(const std::string& name, SourceLocation loc);
        bool  findModuleFile(const std::string& name, std::string& pathOut) const;

        Value callUser(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args, SourceLocation loc);
        Value callLambda(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args, SourceLocation loc);
        Value callListMethod(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args, SourceLocation loc);
        Value callMapMethod(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args, SourceLocation loc);
        Value callTupleMethod(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args, SourceLocation loc);
        Value callSetMethod(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args, SourceLocation loc);
        Value callStringMethod(const std::shared_ptr<Callable>& fn,
            const std::vector<Value>& args, SourceLocation loc);

        void registerStruct(const StructStmt* d);
        void registerClass(const ClassStmt* d);
        void registerEnum(const EnumStmt* d);

        Value constructInstance(const std::shared_ptr<ClassObject>& cls,
            const std::vector<std::pair<std::string, Value>>& args,
            SourceLocation loc);

        std::shared_ptr<Callable> findMethod(const std::shared_ptr<ClassObject>& cls,
            const std::string& name,
            std::shared_ptr<ClassObject>* definingClass);

        bool  valueIsInstanceOf(const Value& v,
            const std::shared_ptr<ClassObject>& cls) const;
        Value makeException(const std::string& typeName, const std::string& msg);

        void installBuiltins();
        void installMathModule();
        void  installExternStubs(const Block& program);
        void installExceptionClasses();
    };

} // namespace vayu