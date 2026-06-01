#ifndef QUANTRASERVER_PRODUCT_REGISTRY_H
#define QUANTRASERVER_PRODUCT_REGISTRY_H

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

// Forward declaration
class CallData;

namespace quantra {

class ProductRegistry {
public:
    // Factory type: function that creates a CallData and starts it
    using Factory = std::function<void(
        QuantraServer::AsyncService*,
        grpc::ServerCompletionQueue*)>;

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

    // Initialize all products (called once at server startup)
    void initializeAll(QuantraServer::AsyncService* service,
                       grpc::ServerCompletionQueue* cq) {
        for (const auto& [name, factory] : factories_) {
            factory(service, cq);
        }
    }

    // Get registered product names (for logging)
    const std::vector<std::string>& getNames() const { return names_; }

private:
    ProductRegistry() = default;
    std::unordered_map<std::string, Factory> factories_;
    std::vector<std::string> names_;
};

// Macro for easy registration
#define REGISTER_PRODUCT(Name, HandlerClass) \
    static bool _registered_##Name = []() { \
        ::quantra::ProductRegistry::instance().registerProduct(#Name, \
            [](QuantraServer::AsyncService* service, \
               grpc::ServerCompletionQueue* cq) { \
                auto handler = new HandlerClass(service, cq); \
                handler->start(); \
            }); \
        return true; \
    }()

} // namespace quantra

#endif