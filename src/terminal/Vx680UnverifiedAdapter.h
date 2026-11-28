#pragma once

#include "ITerminalAdapter.h"

namespace paybridge::terminal
{

// Placeholder for the Verifone VX 680. The real ECR protocol has not been
// verified (payment application, protocol, interface and manual are unknown),
// so this adapter never sends bytes and always reports a definite failure.
// Replace it with the real adapter only once the protocol is confirmed.
class Vx680UnverifiedAdapter final : public ITerminalAdapter
{
public:
    static constexpr const char* ID = "paybridge.verifone.vx680.unverified";

    std::string adapterId() const override;
    AdapterMetadata metadata() const override;
    bool supports(const TerminalModel& terminal) const override;
    AdapterPreflightResult preflight(
        const TerminalModel& terminal,
        const transport::ITransport& transport) const override;
    TerminalOperationResult execute(const TerminalOperationRequest& request,
        transport::ITransport& transport, const BeforeSend& beforeSend) const override;
};

} // namespace paybridge::terminal
