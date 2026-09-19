#pragma once
#include "Value.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace vayu {

    class Environment : public std::enable_shared_from_this<Environment> {
    public:
        explicit Environment(std::shared_ptr<Environment> parent = nullptr)
            : parent_(std::move(parent)) {
        }

        void define(const std::string& name, Value v) {
            auto it = vars_.find(name);
            if (it != vars_.end()) {
                *it->second = std::move(v);
            }
            else {
                vars_[name] = std::make_shared<Value>(std::move(v));
            }
        }

        Value* lookup(const std::string& name) {
            auto it = vars_.find(name);
            if (it != vars_.end()) return it->second.get();
            if (parent_) return parent_->lookup(name);
            return nullptr;
        }

        /// Phase 15.2c — return the shared_ptr backing a name so `&name`
        /// can produce a Reference that survives reassignment.
        std::shared_ptr<Value> lookupShared(const std::string& name) {
            auto it = vars_.find(name);
            if (it != vars_.end()) return it->second;
            if (parent_) return parent_->lookupShared(name);
            return nullptr;
        }

        bool assign(const std::string& name, Value v) {
            auto it = vars_.find(name);
            if (it != vars_.end()) {
                *it->second = std::move(v);
                return true;
            }
            if (parent_) return parent_->assign(name, std::move(v));
            return false;
        }

        std::shared_ptr<Environment> parent() const { return parent_; }

        /// Names bound directly in this scope (not parents).
        const std::unordered_map<std::string, std::shared_ptr<Value>>&
            localVars() const {
            return vars_;
        }

    private:
        std::unordered_map<std::string, std::shared_ptr<Value>> vars_;
        std::shared_ptr<Environment>                            parent_;
    };

} // namespace vayu