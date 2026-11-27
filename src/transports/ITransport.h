#pragma once

#include "TransportResult.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace paybridge::transport
{

class ITransport
{
public:
    virtual ~ITransport() = default;

    virtual TransportResult open() = 0;
    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;

    // True only for an explicitly simulated transport implementation.
    virtual bool isSimulationTransport() const noexcept { return false; }

    virtual TransportResult send(const std::vector<std::uint8_t>& bytes) = 0;
    virtual TransportBytes receive(std::size_t maximumBytes) = 0;

    virtual void setReceiveTimeoutMilliseconds(std::uint32_t milliseconds) noexcept = 0;
    virtual void setSendTimeoutMilliseconds(std::uint32_t milliseconds) noexcept = 0;

    virtual std::string description() const = 0;
};

} // namespace paybridge::transport
