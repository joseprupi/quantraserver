#ifndef QUANTRA_ERROR_H
#define QUANTRA_ERROR_H

#include <exception>
#include <sstream>
#include <string>
#include <memory>

class QuantraError : public std::exception
{
public:
    // Add more detail to the exception
    QuantraError(const std::string &message = "");
    ~QuantraError() throw() {}
    const char *what() const throw() { return message_->c_str(); }

private:
    std::shared_ptr<std::string> message_;
};

inline void QUANTRA_ERROR(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraError(msg_stream.str());
};

#endif //QUANTRA_ERROR_H