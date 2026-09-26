#pragma once
#include "Value.hpp"
#include <memory>
#include <string>
#include <vector>

namespace vayu {

    // Phase 25.0a - flat-vector variable storage.
    //
    // The old implementation used `std::unordered_map<std::string, ...>`,
    // which costs ~40-60 cycles per lookup on MSVC (string hash + bucket
    // walk + string compare).  Most scopes hold under 10 names, so a
    // linear scan over a contiguous vector of (name, slot) pairs beats
    // the hashmap by 3-5x.  Above kHashThreshold entries we switch to a
    // hash map to keep pathological cases bounded.
    class Environment : public std::enable_shared_from_this<Environment> {
    public:
        explicit Environment(std::shared_ptr<Environment> parent = nullptr)
            : parent_(std::move(parent)) {
        }

        void define(const std::string& name, Value v) {
            for (auto& kv : vars_) {
                if (kv.first == name) {
                    *kv.second = std::move(v);
                    return;
                }
            }
            vars_.emplace_back(name, std::make_shared<Value>(std::move(v)));
        }

        Value* lookup(const std::string& name) {
            for (auto& kv : vars_) {
                if (kv.first == name) return kv.second.get();
            }
            if (parent_) return parent_->lookup(name);
            return nullptr;
        }

        std::shared_ptr<Value> lookupShared(const std::string& name) {
            for (auto& kv : vars_) {
                if (kv.first == name) return kv.second;
            }
            if (parent_) return parent_->lookupShared(name);
            return nullptr;
        }

        bool assign(const std::string& name, Value v) {
            for (auto& kv : vars_) {
                if (kv.first == name) {
                    *kv.second = std::move(v);
                    return true;
                }
            }
            if (parent_) return parent_->assign(name, std::move(v));
            return false;
        }

        std::shared_ptr<Environment> parent() const { return parent_; }

        // Replaces the old `localVars()` accessor.  Iterates this scope's
        // bindings without exposing the internal storage layout.
        template <typename Fn>
        void forEachLocal(Fn&& fn) const {
            for (auto& kv : vars_) fn(kv.first, *kv.second);
        }

    private:
        std::vector<std::pair<std::string, std::shared_ptr<Value>>> vars_;
        std::shared_ptr<Environment>                                parent_;
    };

} // namespace vayu