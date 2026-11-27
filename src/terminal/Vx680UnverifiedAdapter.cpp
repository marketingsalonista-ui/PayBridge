#include "Vx680UnverifiedAdapter.h"

namespace paybridge::terminal
{

std::string Vx680UnverifiedAdapter::adapterId() const
{
    return ID;
}

bool Vx680UnverifiedAdapter::supports(const TerminalModel& terminal) const
{
    const bool idMatches = terminal.adapterId.empty() || terminal.adapterId == ID;
    return idMatches &&
        terminal.manufacturer == "Verifone" &&
        terminal.model == "VX 680" &&
        terminal.transportKind != TransportKind::SIMULATED;
}

TerminalOperationResult Vx680UnverifiedAdapter::execute(
    const TerminalOperationRequest& request,
    transport::ITransport& /*transport*/,
    const BeforeSend& /*beforeSend*/) const
{
    // Deliberately does not call beforeSend or touch the transport: nothing is
    // sent, so the request cannot have reached the terminal.
    TerminalOperationResult result;
    result.outcome = TerminalOperationOutcome::FAILED;
    result.requestId = request.requestId;
    result.transactionId = request.transactionId;
    result.externalReference = request.externalReference;
    result.terminalId = request.terminalId;
    result.resultCode = "VX680_PROTOCOL_NOT_VERIFIED";
    result.message = "VX 680 protocol is not verified; no request was sent";
    result.simulated = false;
    result.requestMayHaveBeenSent = false;
    return result;
}

} // namespace paybridge::terminal