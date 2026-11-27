#pragma once

#include "ITerminalAdapter.h"

namespace paybridge::terminal
{

class VerifoneVx680SimulatorAdapter final : public ITerminalAdapter
{
public:
    static constexpr const char* ID =
        "paybridge.verifone.vx680.simulator.v1";

    std::string adapterId() const override;
    bool supports(const TerminalModel& terminal) const override;
    AdapterPreflightResult preflight(
        const TerminalModel& terminal,
        const transport::ITransport& transport) const override;

    TerminalOperationResult execute(
        const TerminalOperationRequest& request,
        transport::ITransport& transport,
        const BeforeSend& beforeSend) const override;
};

} // namespace paybridge::terminal