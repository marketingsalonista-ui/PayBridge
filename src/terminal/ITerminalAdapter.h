#pragma once

#include "TerminalModel.h"
#include "TerminalOperation.h"
#include "../transports/ITransport.h"

#include <functional>
#include <string>

namespace paybridge::terminal
{

struct AdapterMetadata
{
    std::string displayName;
    std::string manufacturer;
    std::string deviceModel;
    std::string executionScope;
    std::string verificationStatus;
    std::string description;
};

struct AdapterPreflightResult
{
    bool ready = true;
    std::string resultCode;
    std::string message;
};

// An adapter owns terminal-specific request encoding and response parsing.
// The supplied transport is already open and provides byte communication only.
// Implementations may be called concurrently for different terminal IDs and
// must keep any shared adapter state thread-safe.
class ITerminalAdapter
{
public:
    using BeforeSend = std::function<bool()>;

    virtual ~ITerminalAdapter() = default;

    virtual std::string adapterId() const = 0;

    // Descriptive, read-only metadata. It does not affect adapter selection
    // or payment execution.
    virtual AdapterMetadata metadata() const
    {
        return {
            adapterId(),
            "Unknown",
            "Unknown",
            "UNSPECIFIED",
            "UNVERIFIED",
            "No adapter metadata supplied"
        };
    }

    virtual bool supports(const TerminalModel& terminal) const = 0;

    // Called before the manager inspects or opens the transport.
    // Existing adapters are ready by default.
    virtual AdapterPreflightResult preflight(
        const TerminalModel&,
        const transport::ITransport&) const
    {
        return {};
    }

    // Called while the selected terminal's operation lock is held. Call
    // beforeSend immediately before the first send attempt. Set
    // requestMayHaveBeenSent when any request bytes may have reached the peer.
    virtual TerminalOperationResult execute(
        const TerminalOperationRequest& request,
        transport::ITransport& transport,
        const BeforeSend& beforeSend) const = 0;
};

} // namespace paybridge::terminal
