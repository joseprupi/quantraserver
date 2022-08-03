// #include "error.h"

// QuantraError::QuantraError(const std::string &message)
// {
//     message_ = std::make_shared<std::string>(message);
// }

// // const char *QuantraError::what() const
// // {
// //     return message_->c_str();
// // }

#include "error.h"

QuantraError::QuantraError(const std::string &message)
{
    message_ = std::make_shared<std::string>(message);
}