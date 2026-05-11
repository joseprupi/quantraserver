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

// Client-error subclasses. Handlers throw these (via the macros below) when
// the request itself is malformed or asks for something that doesn't exist;
// CallDataGeneric translates them to gRPC INVALID_ARGUMENT (HTTP 400) and
// NOT_FOUND (HTTP 404) respectively. Existing handlers that throw the base
// QuantraError continue to surface as ABORTED (HTTP 500), unchanged.
class QuantraInvalidArgument : public QuantraError
{
public:
    explicit QuantraInvalidArgument(const std::string &message = "")
        : QuantraError(message) {}
};

class QuantraNotFound : public QuantraError
{
public:
    explicit QuantraNotFound(const std::string &message = "")
        : QuantraError(message) {}
};

inline void QUANTRA_ERROR(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraError(msg_stream.str());
};

inline void QUANTRA_INVALID_ARGUMENT(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraInvalidArgument(msg_stream.str());
};

inline void QUANTRA_NOT_FOUND(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraNotFound(msg_stream.str());
};

#endif //QUANTRA_ERROR_H