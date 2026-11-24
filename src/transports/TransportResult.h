#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace paybridge::transport
{

enum class TransportError
{
    NONE,
    NOT_OPEN,
    ALREADY_OPEN,
    CONNECT_FAILED,
    DISCONNECTED,
    TIMEOUT,
    INVALID_ARGUMENT,
    SIZE_LIMIT,
    MALFORMED_RESPONSE,
    PLATFORM_ERROR
};

struct TransportResult
{
    bool success = false;
    TransportError error = TransportError::NONE;
    std::string message;
};

struct TransportBytes
{
    TransportResult result;
    std::vector<std::uint8_t> data;
};

std::string transportErrorToString(TransportError error);

} // namespace paybridge::transport
