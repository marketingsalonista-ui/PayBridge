#include "../src/terminal/Vx680UnverifiedAdapter.h"
#include "../src/transports/ITransport.h"

#include <iostream>
#include <string>
#include <vector>

using namespace paybridge::terminal;

namespace
{

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

// Counts every call so the test can prove the adapter never touches the transport.
class CountingTransport final : public paybridge::transport::ITransport
{
public:
    paybridge::transport::TransportResult open() override { ++openCalls; return {true}; }
    void close() noexcept override {}
    bool isOpen() const noexcept override { return true; }
    paybridge::transport::TransportResult send(const std::vector<std::uint8_t>&) override
    {
        ++sendCalls;
        return {true};
    }
    paybridge::transport::TransportBytes receive(std::size_t) override
    {
        ++receiveCalls;
        return {{false, paybridge::transport::TransportError::DISCONNECTED, "unused"}, {}};
    }
    void setReceiveTimeoutMilliseconds(std::uint32_t) noexcept override {}
    void setSendTimeoutMilliseconds(std::uint32_t) noexcept override {}
    std::string description() const override { return "counting test transport"; }

    int openCalls = 0;
    int sendCalls = 0;
    int receiveCalls = 0;
};

TerminalModel vx680(TransportKind kind)
{
    TerminalModel model;
    model.terminalId = "VX680-TEST";
    model.manufacturer = "Verifone";
    model.model = "VX 680";
    model.transportKind = kind;
    return model;
}

} // namespace

int main()
{
    Vx680UnverifiedAdapter adapter;

    check(adapter.adapterId() == Vx680UnverifiedAdapter::ID, "adapter id");

    check(adapter.supports(vx680(TransportKind::TCP)), "supports VX 680 over TCP");
    check(adapter.supports(vx680(TransportKind::SERIAL)), "supports VX 680 over serial");
    check(!adapter.supports(vx680(TransportKind::SIMULATED)), "rejects simulated transport");

    TerminalModel explicitId = vx680(TransportKind::TCP);
    explicitId.adapterId = Vx680UnverifiedAdapter::ID;
    check(adapter.supports(explicitId), "supports its own explicit adapter id");

    TerminalModel otherId = vx680(TransportKind::TCP);
    otherId.adapterId = "paybridge.simulated.v1";
    check(!adapter.supports(otherId), "rejects another adapter's id");

    TerminalModel otherModel = vx680(TransportKind::TCP);
    otherModel.model = "VX 520";
    check(!adapter.supports(otherModel), "rejects other models");

    TerminalModel otherMaker = vx680(TransportKind::TCP);
    otherMaker.manufacturer = "Ingenico";
    check(!adapter.supports(otherMaker), "rejects other manufacturers");

    // execute(): fails closed, copies correlation fields, sends nothing.
    TerminalOperationRequest request;
    request.requestId = "REQ-1";
    request.transactionId = "TXN-1";
    request.externalReference = "EXT-1";
    request.terminalId = "VX680-TEST";

    CountingTransport transport;
    int beforeSendCalls = 0;
    const auto result = adapter.execute(request, transport,
        [&beforeSendCalls] { ++beforeSendCalls; return true; });

    check(result.outcome == TerminalOperationOutcome::FAILED, "execute outcome is FAILED");
    check(result.resultCode == "VX680_PROTOCOL_NOT_VERIFIED", "execute result code");
    check(!result.requestMayHaveBeenSent, "execute: request cannot have been sent");
    check(!result.simulated, "execute: result is not marked simulated");
    check(result.requestId == request.requestId, "execute copies requestId");
    check(result.transactionId == request.transactionId, "execute copies transactionId");
    check(result.externalReference == request.externalReference, "execute copies externalReference");
    check(result.terminalId == request.terminalId, "execute copies terminalId");
    check(beforeSendCalls == 0, "execute never calls beforeSend");
    check(transport.openCalls == 0, "execute never opens the transport");
    check(transport.sendCalls == 0, "execute never sends");
    check(transport.receiveCalls == 0, "execute never receives");

    if (failures == 0)
        std::cout << "Vx680UnverifiedAdapterTests passed\n";
    return failures == 0 ? 0 : 1;
}