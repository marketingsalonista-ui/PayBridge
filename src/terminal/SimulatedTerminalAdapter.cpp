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
AdapterMetadata SimulatedTerminalAdapter::metadata() const
{
    return {
        "PayBridge Simulator",
        "PayBridge",
        "Simulated Terminal",
        "SIMULATION_ONLY",
        "SIMULATED",
        "PayBridge framed simulator; does not communicate with physical payment terminals."
    };
}

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
    // Read the 4-byte frame header, then the declared payload.
    std::vector<std::uint8_t> responseFrame;
    responseFrame.reserve(65540);

    while (responseFrame.size() < 4)
    {
        const std::size_t remaining = 4 - responseFrame.size();
        const auto received = byteTransport.receive(remaining);

        if (!received.result.success || received.data.empty() ||
            received.data.size() > remaining)
            return resultFor(request, TerminalOperationOutcome::UNKNOWN,
                "SIMULATED_RESPONSE_HEADER_INCOMPLETE",
                "response header is incomplete", true);

        responseFrame.insert(responseFrame.end(),
            received.data.begin(), received.data.end());
    }

    const std::vector<std::uint8_t> header(
        responseFrame.begin(), responseFrame.begin() + 4);
    const auto frameLength = SimulatedTerminalProtocol::decodeFrameLength(header);

    if (!frameLength)
        return resultFor(request, TerminalOperationOutcome::UNKNOWN,
            "SIMULATED_RESPONSE_INVALID_LENGTH",
            "response frame length is invalid", true);

    const std::size_t expectedSize = 4 + *frameLength;

    while (responseFrame.size() < expectedSize)
    {
        const std::size_t remaining = expectedSize - responseFrame.size();
        const auto received = byteTransport.receive(remaining);

        if (!received.result.success || received.data.empty() ||
            received.data.size() > remaining)
            return resultFor(request, TerminalOperationOutcome::UNKNOWN,
                "SIMULATED_RESPONSE_BODY_INCOMPLETE",
                "response body is incomplete", true);

        responseFrame.insert(responseFrame.end(),
            received.data.begin(), received.data.end());
    }

    const auto response =
        SimulatedTerminalProtocol::decodeResponse(responseFrame);
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
