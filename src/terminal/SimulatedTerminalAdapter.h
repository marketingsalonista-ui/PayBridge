#pragma once

#include "ITerminalAdapter.h"

namespace paybridge::terminal
{

class SimulatedTerminalAdapter final : public ITerminalAdapter
{
public:
    static constexpr const char* ID = "paybridge.simulated.v1";

    std::string adapterId() const override;
    AdapterMetadata metadata() const override;
    bool supports(const TerminalModel& terminal) const override;
    TerminalOperationResult execute(const TerminalOperationRequest& request,
        transport::ITransport& transport, const BeforeSend& beforeSend) const override;
};

} // namespace paybridge::terminal
