#include "SimulatedTerminalAdapter.h"

#include "SimulatedTerminalProtocol.h"
#include "../money/AmountParser.h"

namespace paybridge::terminal
{
namespace
{
TerminalOperationResult resultFor(const TerminalOperationRequest& request,
    TerminalOperationOutcome outcome, const std::string& code,
    const std::string& message, bool mayHaveBeenSent)
{
    return {outcome, request.requestId, request.transactionId,
        request.externalReference, request.terminalId, code,
        "SIMULATED: " + message, true, mayHaveBeenSent};
}
}

std::string SimulatedTerminalAdapter::adapterId() const { return ID; }

bool SimulatedTerminalAdapter::supports(const TerminalModel& terminal) const
{
    return terminal.transportKind == TransportKind::SIMULATED &&
        (terminal.adapterId.empty() || terminal.adapterId == ID);
}

TerminalOperationResult SimulatedTerminalAdapter::execute(
    const TerminalOperationRequest& request, transport::ITransport& byteTransport,
    const BeforeSend& beforeSend) const
{
    if (!request.isValid())
        return resultFor(request, TerminalOperationOutcome::FAILED,
            "INVALID_OPERATION", "operation request failed validation", false);

    SimulatedTerminalRequest simulatedRequest;
    simulatedRequest.requestId = request.requestId;
    simulatedRequest.transactionId = request.transactionId;
    simulatedRequest.externalReference = request.externalReference;
    simulatedRequest.terminalId = request.terminalId;
    simulatedRequest.amount = paybridge::money::formatAmount(request.amount);
    simulatedRequest.assetIdentity = request.asset.identityKey();
    const auto payload = SimulatedTerminalProtocol::encodeRequest(simulatedRequest);
    SimulatedTerminalProtocol protocol;
    if (!protocol.validateRequest(simulatedRequest, payload))
        return resultFor(request, TerminalOperationOutcome::FAILED,
            "INVALID_SIMULATED_REQUEST", "request failed simulator validation", false);

    if (!beforeSend || !beforeSend())
        return resultFor(request, TerminalOperationOutcome::FAILED,
            "PERSISTENCE_FAILURE", "pre-send persistence was rejected", false);

    // Conservatively mark the outcome uncertain as soon as send is attempted.
    const auto sent = byteTransport.send(payload);
    if (!sent.success)
        return resultFor(request, TerminalOperationOutcome::UNKNOWN,
            "SIMULATED_TRANSPORT_FAILURE", "request send outcome is uncertain", true);
    const auto received = byteTransport.receive(65536);
    if (!received.result.success)
        return resultFor(request, TerminalOperationOutcome::UNKNOWN,
            "SIMULATED_OUTCOME_UNKNOWN", "terminal result is unresolved", true);

    const auto response = SimulatedTerminalProtocol::decodeResponse(received.data);
    if (!response)
        return resultFor(request, TerminalOperationOutcome::UNKNOWN,
            "SIMULATED_RESPONSE_INVALID", "terminal response is unresolved", true);
    TerminalOperationResult normalized;
    normalized.outcome = response->outcome == SimulatedTerminalOutcome::APPROVED
        ? TerminalOperationOutcome::APPROVED : TerminalOperationOutcome::DECLINED;
    normalized.requestId = response->requestId;
    normalized.transactionId = response->transactionId;
    normalized.externalReference = response->externalReference;
    normalized.terminalId = response->terminalId;
    normalized.resultCode = normalized.outcome == TerminalOperationOutcome::APPROVED
        ? "SIMULATED_APPROVED" : "SIMULATED_DECLINED";
    normalized.message = response->simulatedResult;
    normalized.simulated = response->simulated;
    normalized.requestMayHaveBeenSent = true;
    if (!normalized.correlatesTo(request))
        return resultFor(request, TerminalOperationOutcome::UNKNOWN,
            "SIMULATED_RESPONSE_UNCORRELATED", "response correlation failed", true);
    return normalized;
}

} // namespace paybridge::terminal
