#include "TransportResult.h"

namespace paybridge::transport
{

std::string transportErrorToString(TransportError error)
{
    switch (error)
    {
        case TransportError::NONE: return "NONE";
        case TransportError::NOT_OPEN: return "NOT_OPEN";
        case TransportError::ALREADY_OPEN: return "ALREADY_OPEN";
        case TransportError::CONNECT_FAILED: return "CONNECT_FAILED";
        case TransportError::DISCONNECTED: return "DISCONNECTED";
        case TransportError::TIMEOUT: return "TIMEOUT";
        case TransportError::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case TransportError::SIZE_LIMIT: return "SIZE_LIMIT";
        case TransportError::MALFORMED_RESPONSE: return "MALFORMED_RESPONSE";
        case TransportError::PLATFORM_ERROR: return "PLATFORM_ERROR";
    }
    return "PLATFORM_ERROR";
}

} // namespace paybridge::transport
