#include "error.h"

QuantraError::QuantraError(const std::string& message)
    : QuantraError(message, std::string(), std::string()) {}

QuantraError::QuantraError(
    const std::string& message, const std::string& details, const std::string& path) {
    message_ = std::make_shared<std::string>(message);
    details_ = std::make_shared<std::string>(details);
    path_ = std::make_shared<std::string>(path);
}
