#ifndef QUANTRASERVER_PRODUCT_REGISTRY_H
#define QUANTRASERVER_PRODUCT_REGISTRY_H

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration
class CallData;

namespace quantra {

class ProductRegistry {
public:
    // Factory type: function that creates a CallData and starts it
    using Factory = std::function<void(QuantraServer::AsyncService*, grpc::ServerCompletionQueue*)>;

    // Singleton: Create only one and get only one registry
    static ProductRegistry& instance() {
        static ProductRegistry registry;
        return registry;
    }

    // Register a product
    void registerProduct(const std::string& name, Factory factory) {
        factories_[name] = factory;
        names_.push_back(name);
    }

    // Initialize all products (called once at server startup), except the
    // names in `skip`, which the caller starts on another queue with
    // initialize().
    void initializeAll(
        QuantraServer::AsyncService* service,
        grpc::ServerCompletionQueue* cq,
        const std::vector<std::string>& skip = {}) {
        for (const auto& [name, factory] : factories_) {
            if (std::find(skip.begin(), skip.end(), name) == skip.end()) {
                factory(service, cq);
            }
        }
    }

    // Initialize one registered product on the given queue. Returns false when
    // no product has that name.
    bool initialize(
        const std::string& name,
        QuantraServer::AsyncService* service,
        grpc::ServerCompletionQueue* cq) {
        auto it = factories_.find(name);
        if (it == factories_.end()) {
            return false;
        }
        it->second(service, cq);
        return true;
    }

    // Get registered product names (for logging)
    const std::vector<std::string>& getNames() const { return names_; }

private:
    ProductRegistry() = default;
    std::unordered_map<std::string, Factory> factories_;
    std::vector<std::string> names_;
};

// Macro for easy registration
#define REGISTER_PRODUCT(Name, HandlerClass)                                                       \
    static bool _registered_##Name = []() {                                                        \
        ::quantra::ProductRegistry::instance().registerProduct(                                    \
            #Name, [](QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) {     \
                auto handler = new HandlerClass(service, cq);                                      \
                handler->start();                                                                  \
            });                                                                                    \
        return true;                                                                               \
    }()

} // namespace quantra

#endif