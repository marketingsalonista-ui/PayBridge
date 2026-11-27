#include "../terminal/VerifoneVx680SimulatorAdapter.h"
#include "../terminal/SimulatedTerminalTransport.h"
#include "../terminal/TerminalModel.h"
#include "../terminal/TerminalOperation.h"
#include "../transports/ITransport.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace paybridge;
using namespace paybridge::terminal;
using namespace paybridge::transport;

namespace
{

const char* outcomeName(TerminalOperationOutcome outcome)
{
    switch (outcome)
    {
    case TerminalOperationOutcome::APPROVED: return "APPROVED";
    case TerminalOperationOutcome::DECLINED: return "DECLINED";
    case TerminalOperationOutcome::FAILED:   return "FAILED";
    case TerminalOperationOutcome::UNKNOWN:  return "UNKNOWN";
    }
    return "UNRECOGNIZED";
}

TerminalModel makeVerifoneSimulatorProfile()
{
    TerminalModel terminal;
    terminal.manufacturer = "Verifone";
    terminal.model = "VX 680";
    terminal.transportKind = TransportKind::SIMULATED;
    terminal.adapterId = VerifoneVx680SimulatorAdapter::ID;
    return terminal;
}

TerminalOperationRequest makeDemoRequest(const std::string& suffix)
{
    TerminalOperationRequest request;
    request.requestId = "demo-request-" + suffix;
    request.transactionId = "PB-DEMO-" + suffix;
    request.externalReference = "DEMO-" + suffix;
    request.terminalId = "VX680-SIM-DEMO";

    // Fictional AED 12.34. No cardholder or card data is used.
    request.amount = {1234, 2};
    request.asset.type = assets::AssetType::FIAT;
    request.asset.code = "AED";
    request.asset.decimalPrecision = 2;
    return request;
}

void runSimulatedScenario(
    const char* label,
    SimulatedTerminalOutcome configuredOutcome,
    const std::string& suffix)
{
    std::cout << "\n========== " << label << " ==========\n";

    auto terminal = makeVerifoneSimulatorProfile();
    auto request = makeDemoRequest(suffix);
    VerifoneVx680SimulatorAdapter adapter;
    SimulatedTerminalTransport transport(configuredOutcome);

    const auto preflight = adapter.preflight(terminal, transport);
    std::cout << "Preflight: " << (preflight.ready ? "READY" : "REJECTED")
              << '\n';

    if (!preflight.ready)
    {
        std::cout << "Reason: " << preflight.resultCode << '\n';
        std::cout << preflight.message << '\n';
        return;
    }

    const auto opened = transport.open();
    if (!opened.success)
    {
        std::cout << "Could not open in-memory simulator transport.\n";
        return;
    }

    const auto result = adapter.execute(
        request,
        transport,
        []() { return true; });

    std::cout << "Terminal profile: Verifone VX 680 (SIMULATOR)\n";
    std::cout << "Amount: AED 12.34\n";
    std::cout << "Outcome: " << outcomeName(result.outcome) << '\n';
    std::cout << "Result code: " << result.resultCode << '\n';
    std::cout << "Message: " << result.message << '\n';
    std::cout << "Simulated: " << (result.simulated ? "YES" : "NO") << '\n';
    std::cout << "Request may have been sent: "
              << (result.requestMayHaveBeenSent ? "YES" : "NO") << '\n';
    std::cout << "Correlation valid: "
              << (result.correlatesTo(request) ? "YES" : "NO") << '\n';

    transport.close();
}

// This fake deliberately is NOT a simulator transport.
// Its open/send methods count calls; neither should be called by preflight
// or by the simulator adapter's defensive execute check.
class NonSimulationTransport final : public ITransport
{
public:
    TransportResult open() override
    {
        ++openCalls;
        return {};
    }

    void close() noexcept override {}

    bool isOpen() const noexcept override { return false; }

    TransportResult send(const std::vector<std::uint8_t>&) override
    {
        ++sendCalls;
        return {};
    }

    TransportBytes receive(std::size_t) override
    {
        return {};
    }

    void setReceiveTimeoutMilliseconds(std::uint32_t) noexcept override {}
    void setSendTimeoutMilliseconds(std::uint32_t) noexcept override {}

    std::string description() const override
    {
        return "Demo fake non-simulation transport (no hardware)";
    }

    int openCalls = 0;
    int sendCalls = 0;
};

void runPhysicalTransportGuardDemo()
{
    std::cout << "\n========== NON-SIMULATED TRANSPORT GUARD ==========\n";

    auto terminal = makeVerifoneSimulatorProfile();
    auto request = makeDemoRequest("GUARD");
    VerifoneVx680SimulatorAdapter adapter;
    NonSimulationTransport transport;

    const auto preflight = adapter.preflight(terminal, transport);

    std::cout << "Preflight: " << (preflight.ready ? "READY" : "REJECTED")
              << '\n';
    std::cout << "Result code: " << preflight.resultCode << '\n';
    std::cout << "Message: " << preflight.message << '\n';

    // Also demonstrate the adapter's independent execute-time guard.
    const auto result = adapter.execute(
        request,
        transport,
        []() { return true; });

    std::cout << "Execute result: " << outcomeName(result.outcome) << '\n';
    std::cout << "Execute result code: " << result.resultCode << '\n';
    std::cout << "Transport open calls: " << transport.openCalls << '\n';
    std::cout << "Transport send calls: " << transport.sendCalls << '\n';

    if (transport.openCalls == 0 && transport.sendCalls == 0)
        std::cout << "PASS: no transport open or send occurred.\n";
    else
        std::cout << "FAIL: unexpected transport interaction detected.\n";
}

} // namespace

int main()
{
    std::cout << "PayBridge - Verifone VX 680 Simulator Demo\n";
    std::cout << "==========================================\n";
    std::cout << "Internal simulation only. No physical terminal,\n";
    std::cout << "processor, network, database, or live payment is used.\n";

    runSimulatedScenario(
        "SIMULATED APPROVAL",
        SimulatedTerminalOutcome::APPROVED,
        "APPROVED-001");

    runSimulatedScenario(
        "SIMULATED DECLINE",
        SimulatedTerminalOutcome::DECLINED,
        "DECLINED-001");

    runPhysicalTransportGuardDemo();

    std::cout << "\nDemo complete.\n";
    return 0;
}