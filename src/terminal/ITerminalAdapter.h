#pragma once

#include "TerminalModel.h"
#include "TerminalOperation.h"
#include "../transports/ITransport.h"

#include <functional>
#include <string>

namespace paybridge::terminal
{

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
    virtual bool supports(const TerminalModel& terminal) const = 0;

    // Called while the selected terminal's operation lock is held. Call
    // beforeSend immediately before the first send attempt. Set
    // requestMayHaveBeenSent when any request bytes may have reached the peer.
    virtual TerminalOperationResult execute(
        const TerminalOperationRequest& request,
        transport::ITransport& transport,
        const BeforeSend& beforeSend) const = 0;
};

} // namespace paybridge::terminal
