#include "Bte/Core/Result.h"

namespace bte::core {

Error makeError(const ErrorCode code, std::string message, std::source_location where) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    error.where = where;
    return error;
}

} // namespace bte::core
