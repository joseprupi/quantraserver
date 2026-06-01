#include "error.h"

QuantraError::QuantraError(const std::string &message)
{
    message_ = std::make_shared<std::string>(message);
}