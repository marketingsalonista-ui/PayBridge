#include "VerifoneVx680SimulatorAdapter.h"

#include "SimulatedTerminalAdapter.h"

namespace paybridge::terminal
{
namespace
{

bool isSupportedSimulatorProfile(const TerminalModel& terminal)
{
    return terminal.manufacturer == "Verifone" &&
        terminal.model == "VX 680" &&
        terminal.transportKind == TransportKind::SIMULATED &&
        terminal.adapterId == VerifoneVx680SimulatorAdapter::ID;
}

TerminalOperationResult rejectedResult(
    const TerminalOperationRequest& request,
    const std::string& code,
    const std::string& message)
{
    return {
        TerminalOperationOutcome::FAILED,
        request.requestId,
        request.transactionId,
        request.externalReference,
        request.terminalId,
        code,
        message,
        true,
        false
    };
}

} // namespace

std::string VerifoneVx680SimulatorAdapter::adapterId() const
{
    return ID;
}

bool VerifoneVx680SimulatorAdapter::supports(
    const TerminalModel& terminal) const
{
    return isSupportedSimulatorProfile(terminal);
}

AdapterPreflightResult VerifoneVx680SimulatorAdapter::preflight(
    const TerminalModel& terminal,
    const transport::ITransport& byteTransport) const
{
    if (!isSupportedSimulatorProfile(terminal))
    {
        return {
            false,
            "VERIFONE_SIMULATOR_PROFILE_INVALID",
            "Verifone VX 680 simulator profile is invalid; transport was not accessed"
        };
    }

    if (!byteTransport.isSimulationTransport())
    {
        return {
            false,
            "VERIFONE_SIMULATOR_TRANSPORT_REQUIRED",
            "Verifone VX 680 simulator requires a simulated transport; transport was not accessed"
        };
    }

    return {};
}

TerminalOperationResult VerifoneVx680SimulatorAdapter::execute(
    const TerminalOperationRequest& request,
    transport::ITransport& byteTransport,
    const BeforeSend& beforeSend) const
{
    // Defense in depth: the manager also checks this capability during preflight.
    if (!byteTransport.isSimulationTransport())
    {
        return rejectedResult(
            request,
            "VERIFONE_SIMULATOR_TRANSPORT_REQUIRED",
            "Verifone VX 680 simulator requires a simulated transport; no data was sent");
    }

    if (!request.isValid())
    {
        return rejectedResult(
            request,
            "INVALID_OPERATION",
            "Simulator operation request failed validation");
    }

    SimulatedTerminalAdapter simulator;

    const auto result = simulator.execute(
        request,
        byteTransport,
        beforeSend);

    // This adapter is strictly a simulator profile. Never return a result
    // that could be mistaken for a verified physical Verifone transaction.
    if (!result.simulated)
    {
        return rejectedResult(
            request,
            "VERIFONE_SIMULATOR_RESULT_INVALID",
            "Simulator returned a result without its simulated marker");
    }

    return result;
}

} // namespace paybridge::terminal