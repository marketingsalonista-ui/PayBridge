#include "../src/terminal/ITerminalAdapter.h"
#include "../src/terminal/SimulatedTerminalAdapter.h"
#include "../src/terminal/SimulatedTerminalTransport.h"
#include "../src/terminal/SimulatedTerminalProtocol.h"
#include "../src/terminal/TerminalManager.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <string>
#include <mutex>

using namespace paybridge::terminal;

namespace
{
TerminalOperationRequest validRequest()
{
    TerminalOperationRequest request;
    request.requestId = "0123456789abcdef0123456789abcdef";
    request.transactionId = "PB-0123456789ABCDEF0123456789ABCDEF";
    request.externalReference = "POS-OPERATION-001";
    request.terminalId = "SIM-TEST-001";
    request.amount = {1234, 2};
    request.asset.type = paybridge::assets::AssetType::FIAT;
    request.asset.code = "AED";
    request.asset.decimalPrecision = 2;
    return request;
}

TerminalOperationResult resultFor(const TerminalOperationRequest& request)
{
    TerminalOperationResult result;
    result.outcome = TerminalOperationOutcome::APPROVED;
    result.requestId = request.requestId;
    result.transactionId = request.transactionId;
    result.externalReference = request.externalReference;
    result.terminalId = request.terminalId;
    result.resultCode = "SIMULATED_APPROVED";
    result.message = "SIMULATED APPROVED - no real payment was processed";
    result.simulated = true;
    result.requestMayHaveBeenSent = true;
    return result;
}

class RealResultTestAdapter final : public ITerminalAdapter
{
public:
    std::string adapterId() const override
    {
        return "test.real-result.v1";
    }

    bool supports(const TerminalModel& terminal) const override
    {
        return terminal.transportKind == TransportKind::SIMULATED;
    }

    TerminalOperationResult execute(const TerminalOperationRequest& request,
        paybridge::transport::ITransport&, const BeforeSend& beforeSend) const override
    {
        if (!beforeSend || !beforeSend()) return {};

        TerminalOperationResult result;
        result.outcome = TerminalOperationOutcome::APPROVED;
        result.requestId = request.requestId;
        result.transactionId = request.transactionId;
        result.externalReference = request.externalReference;
        result.terminalId = request.terminalId;
        result.resultCode = "REAL_APPROVED";
        result.message = "Hardware-style approval";
        result.simulated = false;
        result.requestMayHaveBeenSent = true;
        return result;
    }
};

class SlowTestAdapter final : public ITerminalAdapter
{
public:
    explicit SlowTestAdapter(std::atomic<int>& active, std::atomic<int>& maximum,
        std::string id = "test.serialized.v1", std::atomic<int>* entered = nullptr)
        : active(active), maximum(maximum), id(std::move(id)), entered(entered) {}
    std::string adapterId() const override { return id; }
    bool supports(const TerminalModel& terminal) const override
    { return terminal.transportKind == TransportKind::SIMULATED; }
    TerminalOperationResult execute(const TerminalOperationRequest& request,
        paybridge::transport::ITransport&, const BeforeSend& beforeSend) const override
    {
        if (!beforeSend || !beforeSend()) return {};
        const int now = ++active;
        if (entered) ++(*entered);
        int observed = maximum.load();
        while (now > observed && !maximum.compare_exchange_weak(observed, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        --active;
        TerminalOperationResult result;
        result.outcome = TerminalOperationOutcome::APPROVED;
        result.requestId = request.requestId;
        result.transactionId = request.transactionId;
        result.externalReference = request.externalReference;
        result.terminalId = request.terminalId;
        result.simulated = true;
        result.requestMayHaveBeenSent = true;
        return result;
    }
private:
    std::atomic<int>& active;
    std::atomic<int>& maximum;
    std::string id;
    std::atomic<int>* entered;
};

class TamperedResponseTransport final : public paybridge::transport::ITransport
{
public:
    explicit TamperedResponseTransport(int field, std::size_t chunkSize = 65536)
        : field(field), chunkSize(chunkSize) {}
    paybridge::transport::TransportResult open() override { openState = true; return {true}; }
    void close() noexcept override { openState = false; }
    bool isOpen() const noexcept override { return openState; }
    paybridge::transport::TransportResult send(const std::vector<std::uint8_t>& bytes) override
    {
        auto request = SimulatedTerminalProtocol::decodeRequest(bytes);
        if (!request) return {false, paybridge::transport::TransportError::MALFORMED_RESPONSE, "bad test request"};
        if (field == 0) request->requestId += "x";
        if (field == 1) request->transactionId += "x";
        if (field == 2) request->externalReference += "x";
        if (field == 3) request->terminalId += "x";
        response = SimulatedTerminalProtocol::encodeResponse(*request, SimulatedTerminalOutcome::APPROVED);
        responseOffset = 0;
        return {true};
    }
    paybridge::transport::TransportBytes receive(std::size_t maximum) override
    {
        if (!openState || maximum == 0 || responseOffset >= response.size())
            return {{false, paybridge::transport::TransportError::DISCONNECTED, "no response bytes available"}, {}};

        const std::size_t remaining = response.size() - responseOffset;
        const std::size_t count = (maximum < chunkSize)
            ? ((maximum < remaining) ? maximum : remaining)
            : ((chunkSize < remaining) ? chunkSize : remaining);

        std::vector<std::uint8_t> chunk(
            response.begin() + responseOffset,
            response.begin() + responseOffset + count);
        responseOffset += count;
        return {{true}, std::move(chunk)};
    }
    void setReceiveTimeoutMilliseconds(std::uint32_t) noexcept override {}
    void setSendTimeoutMilliseconds(std::uint32_t) noexcept override {}
    std::string description() const override { return "test response transport"; }
private:
    int field;
    std::size_t chunkSize;
    std::size_t responseOffset = 0;
    bool openState = false;
    std::vector<std::uint8_t> response;
};

class ThrowingOpenTransport final : public paybridge::transport::ITransport
{
public:
    paybridge::transport::TransportResult open() override
    {
        throw std::runtime_error("Injected transport open exception");
    }

    void close() noexcept override {}
    bool isOpen() const noexcept override { return false; }

    paybridge::transport::TransportResult send(
        const std::vector<std::uint8_t>&) override
    {
        return {false, paybridge::transport::TransportError::NOT_OPEN,
            "Transport is not open"};
    }

    paybridge::transport::TransportBytes receive(std::size_t) override
    {
        return {{false, paybridge::transport::TransportError::NOT_OPEN,
            "Transport is not open"}, {}};
    }

    void setReceiveTimeoutMilliseconds(std::uint32_t) noexcept override {}
    void setSendTimeoutMilliseconds(std::uint32_t) noexcept override {}
    std::string description() const override { return "throwing open test transport"; }
};
class ThrowingIoTransport final : public paybridge::transport::ITransport
{
public:
    ThrowingIoTransport(bool throwOnSend, bool throwOnReceive)
        : throwOnSend(throwOnSend), throwOnReceive(throwOnReceive) {}

    paybridge::transport::TransportResult open() override
    {
        openState = true;
        return {true};
    }

    void close() noexcept override { openState = false; }
    bool isOpen() const noexcept override { return openState; }

    paybridge::transport::TransportResult send(
        const std::vector<std::uint8_t>&) override
    {
        ++sendCalls;
        if (throwOnSend)
            throw std::runtime_error("Injected send exception");
        return {true};
    }

    paybridge::transport::TransportBytes receive(std::size_t) override
    {
        if (throwOnReceive)
            throw std::runtime_error("Injected receive exception");
        return {{true}, {0x02}};
    }

    void setReceiveTimeoutMilliseconds(std::uint32_t) noexcept override {}
    void setSendTimeoutMilliseconds(std::uint32_t) noexcept override {}
    std::string description() const override { return "throwing I/O test transport"; }

    int sendCalls = 0;

private:
    bool throwOnSend;
    bool throwOnReceive;
    bool openState = true;
};
class ThrowingTestAdapter final : public ITerminalAdapter
{
public:
    explicit ThrowingTestAdapter(bool callBeforeSend)
        : callBeforeSend(callBeforeSend) {}

    std::string adapterId() const override
    {
        return "test.throwing.v1";
    }

    bool supports(const TerminalModel& terminal) const override
    {
        return terminal.transportKind == TransportKind::SIMULATED;
    }

    TerminalOperationResult execute(
        const TerminalOperationRequest&,
        paybridge::transport::ITransport&,
        const BeforeSend& beforeSend) const override
    {
        if (callBeforeSend && (!beforeSend || !beforeSend()))
            throw std::runtime_error("Pre-send callback rejected");

        throw std::runtime_error("Injected adapter exception");
    }

private:
    bool callBeforeSend;
};

class ContractViolationAdapter final : public ITerminalAdapter
{
public:
    explicit ContractViolationAdapter(
        bool callTwice,
        std::atomic<int>& successfulCallbacks,
        bool unknownWithoutCallback = false)
        : callTwice(callTwice),
          successfulCallbacks(successfulCallbacks),
          unknownWithoutCallback(unknownWithoutCallback) {}std::string adapterId() const override
    {
        return "test.contract.violation.v1";
    }

    bool supports(const TerminalModel& terminal) const override
    {
        return terminal.transportKind == TransportKind::SIMULATED;
    }

    TerminalOperationResult execute(
        const TerminalOperationRequest& request,
        paybridge::transport::ITransport&,
        const BeforeSend& beforeSend) const override
    {
        if (!callTwice)
        {
            auto result = resultFor(request);

            if (unknownWithoutCallback)
            {
                result.outcome = TerminalOperationOutcome::UNKNOWN;
                result.resultCode = "TEST_UNCERTAIN_WITHOUT_CALLBACK";
                result.message = "Test adapter reports possible transmission";
                result.requestMayHaveBeenSent = true;
                return result;
            }

            result.requestMayHaveBeenSent = false;
            return result;
        }

        const bool first = beforeSend && beforeSend();
        const bool second = beforeSend && beforeSend();
        successfulCallbacks = (first ? 1 : 0) + (second ? 1 : 0);

        TerminalOperationResult result;
        result.outcome = TerminalOperationOutcome::FAILED;
        result.requestId = request.requestId;
        result.transactionId = request.transactionId;
        result.externalReference = request.externalReference;
        result.terminalId = request.terminalId;
        result.resultCode = "TEST_CALLBACK_REPEATED";
        result.message = "Test adapter attempted repeated persistence";
        result.simulated = true;
        result.requestMayHaveBeenSent = false;
        return result;
    }

private:
    bool callTwice;
    std::atomic<int>& successfulCallbacks;
    bool unknownWithoutCallback;
};
// Counts send() calls so tests can prove whether bytes reached the transport.
class SendCountingTransport final : public paybridge::transport::ITransport
{
public:
    explicit SendCountingTransport(std::atomic<int>& sendCalls) : sendCalls(sendCalls) {}

    paybridge::transport::TransportResult open() override { openState = true; return {true}; }
    void close() noexcept override { openState = false; }
    bool isOpen() const noexcept override { return openState; }

    paybridge::transport::TransportResult send(
        const std::vector<std::uint8_t>&) override
    {
        ++sendCalls;
        return {true};
    }

    paybridge::transport::TransportBytes receive(std::size_t) override
    {
        return {{true}, {0x02}};
    }

    void setReceiveTimeoutMilliseconds(std::uint32_t) noexcept override {}
    void setSendTimeoutMilliseconds(std::uint32_t) noexcept override {}
    std::string description() const override { return "send-counting test transport"; }

private:
    std::atomic<int>& sendCalls;
    bool openState = false;
};

// Models a non-conforming adapter that calls beforeSend but ignores a false
// (persistence rejected) result and still reports APPROVED.
class PersistenceIgnoringAdapter final : public ITerminalAdapter
{
public:
    explicit PersistenceIgnoringAdapter(bool sendAnyway) : sendAnyway(sendAnyway) {}

    std::string adapterId() const override { return "test.persistence-ignoring.v1"; }

    bool supports(const TerminalModel& terminal) const override
    {
        return terminal.transportKind == TransportKind::SIMULATED;
    }

    TerminalOperationResult execute(const TerminalOperationRequest& request,
        paybridge::transport::ITransport& transport,
        const BeforeSend& beforeSend) const override
    {
        const bool persisted = beforeSend && beforeSend();
        (void)persisted; // Deliberately ignored: this adapter violates the contract.
        if (sendAnyway) transport.send(std::vector<std::uint8_t>{0x01});
        auto result = resultFor(request); // APPROVED
        result.requestMayHaveBeenSent = sendAnyway;
        return result;
    }

private:
    bool sendAnyway;
};

// Retries beforeSend after the first (rejected) attempt. The manager's guard
// must never let a later attempt succeed.
class RetryAfterRejectionAdapter final : public ITerminalAdapter
{
public:
    explicit RetryAfterRejectionAdapter(std::atomic<int>& successfulCallbacks)
        : successfulCallbacks(successfulCallbacks) {}

    std::string adapterId() const override { return "test.retry-after-rejection.v1"; }

    bool supports(const TerminalModel& terminal) const override
    {
        return terminal.transportKind == TransportKind::SIMULATED;
    }

    TerminalOperationResult execute(const TerminalOperationRequest& request,
        paybridge::transport::ITransport&, const BeforeSend& beforeSend) const override
    {
        int successes = 0;
        for (int attempt = 0; attempt < 3; ++attempt)
            if (beforeSend && beforeSend()) ++successes;
        successfulCallbacks = successes;

        auto result = resultFor(request);
        result.outcome = TerminalOperationOutcome::FAILED;
        result.resultCode = "TEST_PERSISTENCE_REJECTED";
        result.requestMayHaveBeenSent = false;
        return result;
    }

private:
    std::atomic<int>& successfulCallbacks;
};

// Sends bytes without ever calling beforeSend, then reports a clean FAILED.
class BeforeSendBypassingAdapter final : public ITerminalAdapter
{
public:
    std::string adapterId() const override { return "test.beforesend-bypass.v1"; }

    bool supports(const TerminalModel& terminal) const override
    {
        return terminal.transportKind == TransportKind::SIMULATED;
    }

    TerminalOperationResult execute(const TerminalOperationRequest& request,
        paybridge::transport::ITransport& transport,
        const BeforeSend&) const override
    {
        transport.send(std::vector<std::uint8_t>{0x01});
        auto result = resultFor(request);
        result.outcome = TerminalOperationOutcome::FAILED;
        result.resultCode = "TEST_SEND_WITHOUT_PERSISTENCE";
        result.requestMayHaveBeenSent = false;
        return result;
    }
};

class BlockingTestAdapter final : public ITerminalAdapter
{
public:
    std::string adapterId() const override
    {
        return "test.blocking.v1";
    }

    bool supports(const TerminalModel& terminal) const override
    {
        return terminal.transportKind == TransportKind::SIMULATED;
    }

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(
            lock, std::chrono::seconds(5), [&] { return entered; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        condition.notify_all();
    }

    TerminalOperationResult execute(
        const TerminalOperationRequest& request,
        paybridge::transport::ITransport& transport,
        const BeforeSend& beforeSend) const override
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return released; });
        }

        SimulatedTerminalAdapter simulator;
        return simulator.execute(request, transport, beforeSend);
    }

private:
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    mutable bool entered = false;
    mutable bool released = false;
};
}

// Serves scripted bytes so framing failures can be tested.
class ScriptedResponseTransport final : public paybridge::transport::ITransport
{
public:
    explicit ScriptedResponseTransport(std::vector<std::uint8_t> bytes,
        std::size_t chunkSize = 65536, bool ignoreMaximum = false,
        bool emptySuccessWhenExhausted = false)
        : script(std::move(bytes)), chunkSize(chunkSize),
          ignoreMaximum(ignoreMaximum), emptySuccessWhenExhausted(emptySuccessWhenExhausted) {}
    paybridge::transport::TransportResult open() override { openState = true; return {true}; }
    void close() noexcept override { openState = false; }
    bool isOpen() const noexcept override { return openState; }
    paybridge::transport::TransportResult send(const std::vector<std::uint8_t>&) override
    {
        return {true};
    }
    paybridge::transport::TransportBytes receive(std::size_t maximum) override
    {
        if (offset >= script.size())
        {
            if (emptySuccessWhenExhausted) return {{true}, {}};
            return {{false, paybridge::transport::TransportError::DISCONNECTED,
                "script exhausted"}, {}};
        }
        std::size_t count = script.size() - offset;
        if (count > chunkSize) count = chunkSize;
        if (!ignoreMaximum && count > maximum) count = maximum;
        std::vector<std::uint8_t> chunk(
            script.begin() + offset, script.begin() + offset + count);
        offset += count;
        return {{true}, std::move(chunk)};
    }
    void setReceiveTimeoutMilliseconds(std::uint32_t) noexcept override {}
    void setSendTimeoutMilliseconds(std::uint32_t) noexcept override {}
    std::string description() const override { return "scripted response transport"; }
private:
    std::vector<std::uint8_t> script;
    std::size_t chunkSize;
    bool ignoreMaximum;
    bool emptySuccessWhenExhausted;
    std::size_t offset = 0;
    bool openState = false;
};

int main()
{
    const TerminalOperationRequest request = validRequest();
    assert(request.isValid());

    TerminalOperationRequest invalid = request;
    invalid.requestId.clear();
    assert(!invalid.isValid());
    invalid = request;
    invalid.transactionId.clear();
    assert(!invalid.isValid());
    invalid = request;
    invalid.externalReference.clear();
    assert(!invalid.isValid());
    invalid = request;
    invalid.terminalId.clear();
    assert(!invalid.isValid());
    invalid = request;
    invalid.amount.units = 0;
    assert(!invalid.isValid());
    invalid = request;
    invalid.amount.precision = 3;
    assert(!invalid.isValid());
    invalid = request;
    invalid.asset.code.clear();
    assert(!invalid.isValid());

    const TerminalOperationResult result = resultFor(request);
    assert(result.correlatesTo(request));
    for (const auto field : {"requestId", "transactionId", "externalReference", "terminalId"})
    {
        TerminalOperationResult mismatched = result;
        if (std::string(field) == "requestId") mismatched.requestId += "x";
        else if (std::string(field) == "transactionId") mismatched.transactionId += "x";
        else if (std::string(field) == "externalReference") mismatched.externalReference += "x";
        else mismatched.terminalId += "x";
        assert(!mismatched.correlatesTo(request));
    }

    assert(result.simulated);
    assert(result.outcome == TerminalOperationOutcome::APPROVED);
    assert(result.message.find("no real payment was processed") != std::string::npos);

    SimulatedTerminalAdapter adapter;
    TerminalModel model;
    model.terminalId = request.terminalId;
    model.transportKind = TransportKind::SIMULATED;
    model.adapterId = SimulatedTerminalAdapter::ID;
    assert(adapter.supports(model));
    model.transportKind = TransportKind::SERIAL;
    assert(!adapter.supports(model));

    auto runOutcome = [&](SimulatedTerminalOutcome outcome)
    {
        SimulatedTerminalTransport transport(outcome);
        assert(transport.open().success);
        bool persistedBeforeSend = false;
        const auto normalized = adapter.execute(request, transport, [&]()
        {
            persistedBeforeSend = true;
            return true;
        });
        assert(persistedBeforeSend);
        assert(normalized.correlatesTo(request));
        return normalized;
    };
    const auto approved = runOutcome(SimulatedTerminalOutcome::APPROVED);
    assert(approved.outcome == TerminalOperationOutcome::APPROVED && approved.simulated);
    const auto declined = runOutcome(SimulatedTerminalOutcome::DECLINED);
    assert(declined.outcome == TerminalOperationOutcome::DECLINED && declined.simulated);
    const auto timedOut = runOutcome(SimulatedTerminalOutcome::TIMEOUT);
    assert(timedOut.outcome == TerminalOperationOutcome::UNKNOWN && timedOut.requestMayHaveBeenSent);
    const auto malformed = runOutcome(SimulatedTerminalOutcome::MALFORMED);
    assert(malformed.outcome == TerminalOperationOutcome::UNKNOWN && malformed.requestMayHaveBeenSent);
    SimulatedTerminalTransport rejectedTransport(SimulatedTerminalOutcome::APPROVED);
    assert(rejectedTransport.open().success);
    const auto notPersisted = adapter.execute(request, rejectedTransport, [] { return false; });
    assert(notPersisted.outcome == TerminalOperationOutcome::FAILED);
    assert(!notPersisted.requestMayHaveBeenSent);
    for (int field = 0; field < 4; ++field)
    {
        TamperedResponseTransport tampered(field);
        assert(tampered.open().success);
        const auto uncorrelated = adapter.execute(request, tampered, [] { return true; });
        assert(uncorrelated.outcome == TerminalOperationOutcome::UNKNOWN);
        assert(uncorrelated.requestMayHaveBeenSent);
    }

    // Verify response assembly when the transport returns 2-byte chunks.
    {
        TamperedResponseTransport fragmented(-1, 2);
        assert(fragmented.open().success);
        const auto fragmentedResult = adapter.execute(
            request, fragmented, [] { return true; });
        assert(fragmentedResult.outcome == TerminalOperationOutcome::APPROVED);
        assert(fragmentedResult.simulated);
        assert(fragmentedResult.correlatesTo(request));
    }

    // Malformed, truncated and oversized response frames must resolve to UNKNOWN.
    {
        const auto expectUnknown = [&](ScriptedResponseTransport& scripted, const char* code)
        {
            const bool opened = scripted.open().success;
            assert(opened);
            const auto result = adapter.execute(request, scripted, [] { return true; });
            assert(result.outcome == TerminalOperationOutcome::UNKNOWN);
            assert(result.resultCode == code);
            assert(result.requestMayHaveBeenSent);
            assert(result.simulated);
            (void)opened;
            (void)result;
            (void)code;
        };

        const std::vector<std::uint8_t> shortHeader = {0x00, 0x00};
        const std::vector<std::uint8_t> zeroLength = {0x00, 0x00, 0x00, 0x00};
        const std::vector<std::uint8_t> tooLarge = {0x00, 0x01, 0x00, 0x01};
        const std::vector<std::uint8_t> maxLengthNoBody = {0x00, 0x01, 0x00, 0x00};
        const std::vector<std::uint8_t> truncatedBody =
            {0x00, 0x00, 0x00, 0x0A, 0x61, 0x62, 0x63};
        const std::string garbageText = "hello";
        const std::vector<std::uint8_t> garbagePayload(garbageText.begin(), garbageText.end());
        const auto garbageFrame = SimulatedTerminalProtocol::framePayload(garbagePayload);

        { ScriptedResponseTransport t(shortHeader);
          expectUnknown(t, "SIMULATED_RESPONSE_HEADER_INCOMPLETE"); }
        { ScriptedResponseTransport t(zeroLength);
          expectUnknown(t, "SIMULATED_RESPONSE_INVALID_LENGTH"); }
        { ScriptedResponseTransport t(tooLarge);
          expectUnknown(t, "SIMULATED_RESPONSE_INVALID_LENGTH"); }
        { ScriptedResponseTransport t(maxLengthNoBody);
          expectUnknown(t, "SIMULATED_RESPONSE_BODY_INCOMPLETE"); }
        { ScriptedResponseTransport t(truncatedBody);
          expectUnknown(t, "SIMULATED_RESPONSE_BODY_INCOMPLETE"); }
        { ScriptedResponseTransport t(garbageFrame);
          expectUnknown(t, "SIMULATED_RESPONSE_INVALID"); }
        { ScriptedResponseTransport t(garbageFrame, 1);
          expectUnknown(t, "SIMULATED_RESPONSE_INVALID"); }
        { ScriptedResponseTransport t(garbageFrame, 65536, true);
          expectUnknown(t, "SIMULATED_RESPONSE_HEADER_INCOMPLETE"); }
        { ScriptedResponseTransport t(std::vector<std::uint8_t>{}, 65536, false, true);
          expectUnknown(t, "SIMULATED_RESPONSE_HEADER_INCOMPLETE"); }
    }

    // Exception before persistence.
    {
        TerminalManager exceptionManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-EXCEPTION-BEFORE";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.throwing.v1";

        assert(exceptionManager.registerAdapter(
            std::make_shared<ThrowingTestAdapter>(false)));
        assert(exceptionManager.registerTerminal(terminal));

        exceptionManager.setTransport(terminal.terminalId,
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::APPROVED));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = exceptionManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return true;
            });

        assert(result.outcome == TerminalOperationOutcome::FAILED);
        assert(!result.requestMayHaveBeenSent);
        assert(persistenceCalls == 0);
    }

    // Exception after persistence.
    {
        TerminalManager exceptionManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-EXCEPTION-AFTER";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.throwing.v1";

        assert(exceptionManager.registerAdapter(
            std::make_shared<ThrowingTestAdapter>(true)));
        assert(exceptionManager.registerTerminal(terminal));

        exceptionManager.setTransport(terminal.terminalId,
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::APPROVED));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = exceptionManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return true;
            });

        assert(result.outcome == TerminalOperationOutcome::UNKNOWN);
        assert(result.requestMayHaveBeenSent);
        assert(persistenceCalls == 1);
    }
    // A genuine (non-simulated) approval is valid after pre-send persistence.
    {
        TerminalManager realResultManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-REAL-RESULT-APPROVAL";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.real-result.v1";

        assert(realResultManager.registerAdapter(
            std::make_shared<RealResultTestAdapter>()));
        assert(realResultManager.registerTerminal(terminal));

        realResultManager.setTransport(terminal.terminalId,
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::APPROVED));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = realResultManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return true;
            });

        assert(result.outcome == TerminalOperationOutcome::APPROVED);
        assert(result.resultCode == "REAL_APPROVED");
        assert(!result.simulated);
        assert(result.requestMayHaveBeenSent);
        assert(persistenceCalls == 1);
    }

    // An approval without the pre-send persistence callback must be rejected.
    {
        TerminalManager contractManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-MISSING-PERSISTENCE";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.contract.violation.v1";

        std::atomic<int> successfulCallbacks{0};
        assert(contractManager.registerAdapter(
            std::make_shared<ContractViolationAdapter>(false, successfulCallbacks)));
        assert(contractManager.registerTerminal(terminal));

        contractManager.setTransport(terminal.terminalId,
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::APPROVED));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = contractManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return true;
            });

        assert(result.outcome == TerminalOperationOutcome::UNKNOWN);
        assert(result.resultCode == "ADAPTER_RESULT_INVALID");
        assert(result.requestMayHaveBeenSent);
        assert(persistenceCalls == 0);
        assert(successfulCallbacks == 0);
    }

    // An UNKNOWN result that may have been transmitted must remain UNKNOWN,
// even when the adapter omitted the pre-send persistence callback.
    {
        TerminalManager contractManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-UNKNOWN-WITHOUT-PERSISTENCE";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.contract.violation.v1";

        std::atomic<int> successfulCallbacks{0};
        assert(contractManager.registerAdapter(
            std::make_shared<ContractViolationAdapter>(
                false, successfulCallbacks, true)));
        assert(contractManager.registerTerminal(terminal));

        contractManager.setTransport(terminal.terminalId,
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::APPROVED));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = contractManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return true;
            });

        assert(result.outcome == TerminalOperationOutcome::UNKNOWN);
        assert(result.resultCode == "ADAPTER_OUTCOME_UNKNOWN");
        assert(result.requestMayHaveBeenSent);
        assert(persistenceCalls == 0);
        assert(successfulCallbacks == 0);
    }
// A repeated beforeSend callback must not succeed twice.
    {
        TerminalManager contractManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-REPEATED-PERSISTENCE";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.contract.violation.v1";

        std::atomic<int> successfulCallbacks{0};
        assert(contractManager.registerAdapter(
            std::make_shared<ContractViolationAdapter>(true, successfulCallbacks)));
        assert(contractManager.registerTerminal(terminal));

        contractManager.setTransport(terminal.terminalId,
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::APPROVED));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = contractManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return true;
            });

        assert(result.outcome == TerminalOperationOutcome::UNKNOWN);
        assert(result.resultCode == "ADAPTER_FAILURE_AFTER_SEND");
        assert(result.requestMayHaveBeenSent);
        assert(persistenceCalls == 1);
        assert(successfulCallbacks == 1);
    }
    // beforeSend is invoked but rejected (persistence failed), yet a
    // non-conforming adapter still reports APPROVED. The manager must never
    // surface that approval, and must stay UNKNOWN rather than FAILED so the
    // caller does not treat the payment as safely retryable.
    for (const bool sendAnyway : {true, false})
    {
        std::atomic<int> sendCalls{0};
        TerminalManager contractManager;
        TerminalModel terminal;
        terminal.terminalId = sendAnyway ? "SIM-PERSIST-REJECTED-SENT" : "SIM-PERSIST-REJECTED-UNSENT";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.persistence-ignoring.v1";

        const bool adapterRegistered = contractManager.registerAdapter(
            std::make_shared<PersistenceIgnoringAdapter>(sendAnyway));
        const bool terminalRegistered = contractManager.registerTerminal(terminal);
        assert(adapterRegistered && terminalRegistered);
        (void)adapterRegistered; (void)terminalRegistered;
        contractManager.setTransport(terminal.terminalId,
            std::make_unique<SendCountingTransport>(sendCalls));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = contractManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return false;
            });

        assert(persistenceCalls == 1);
        assert(result.outcome == TerminalOperationOutcome::UNKNOWN);
        assert(result.resultCode == "ADAPTER_RESULT_INVALID");
        assert(result.requestMayHaveBeenSent);
        assert(result.correlatesTo(testRequest));
        // The transport guard blocks sends when persistence is rejected.
        assert(sendCalls == 0);
    }

    // A rejected persistence attempt must not be retryable: later beforeSend
    // calls fail without re-invoking the persistence callback.
    {
        std::atomic<int> sendCalls{0};
        std::atomic<int> successfulCallbacks{-1};
        TerminalManager contractManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-PERSIST-NO-RETRY";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.retry-after-rejection.v1";

        const bool adapterRegistered = contractManager.registerAdapter(
            std::make_shared<RetryAfterRejectionAdapter>(successfulCallbacks));
        const bool terminalRegistered = contractManager.registerTerminal(terminal);
        assert(adapterRegistered && terminalRegistered);
        (void)adapterRegistered; (void)terminalRegistered;
        contractManager.setTransport(terminal.terminalId,
            std::make_unique<SendCountingTransport>(sendCalls));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = contractManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return false;
            });

        assert(persistenceCalls == 1);
        assert(successfulCallbacks == 0);
        assert(sendCalls == 0);
        assert(result.outcome == TerminalOperationOutcome::FAILED);
        assert(!result.requestMayHaveBeenSent);
    }

    // A non-conforming adapter cannot bypass pre-send persistence by sending
    // directly through the transport.
    {
        std::atomic<int> sendCalls{0};
        TerminalManager contractManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-BEFORESEND-BYPASS";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = "test.beforesend-bypass.v1";

        const bool adapterRegistered = contractManager.registerAdapter(
            std::make_shared<BeforeSendBypassingAdapter>());
        const bool terminalRegistered = contractManager.registerTerminal(terminal);
        assert(adapterRegistered && terminalRegistered);
        (void)adapterRegistered; (void)terminalRegistered;
        contractManager.setTransport(terminal.terminalId,
            std::make_unique<SendCountingTransport>(sendCalls));

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        const auto result = contractManager.executeOperation(
            testRequest, [&]()
            {
                ++persistenceCalls;
                return true;
            });

        assert(persistenceCalls == 0);
        assert(sendCalls == 0);
        assert(result.outcome == TerminalOperationOutcome::FAILED);
        assert(!result.requestMayHaveBeenSent);
    }

    // A transport open exception must not escape the operation boundary.
    {
        TerminalManager openExceptionManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-OPEN-EXCEPTION";
        terminal.transportKind = TransportKind::SIMULATED;
        terminal.adapterId = SimulatedTerminalAdapter::ID;

        assert(openExceptionManager.registerAdapter(
            std::make_shared<SimulatedTerminalAdapter>()));
        assert(openExceptionManager.registerTerminal(terminal));

        openExceptionManager.setTransport(terminal.terminalId,
            std::make_unique<ThrowingOpenTransport>());

        auto testRequest = request;
        testRequest.terminalId = terminal.terminalId;

        int persistenceCalls = 0;
        bool exceptionEscaped = false;

        try
        {
            const auto result = openExceptionManager.executeOperation(
                testRequest, [&]()
                {
                    ++persistenceCalls;
                    return true;
                });
            (void)result;
        }
        catch (...)
        {
            exceptionEscaped = true;
        }

        assert(!exceptionEscaped);
        assert(persistenceCalls == 0);
    }
    // Public connect() converts an open exception into a safe failure.
    {
        TerminalManager testManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-CONNECT-EXCEPTION";
        terminal.transportKind = TransportKind::SIMULATED;
        assert(testManager.registerTerminal(terminal));
        testManager.setTransport(terminal.terminalId,
            std::make_unique<ThrowingOpenTransport>());

        bool escaped = false;
        paybridge::transport::TransportResult result;
        try { result = testManager.connect(terminal.terminalId); }
        catch (...) { escaped = true; }

        assert(!escaped);
        assert(!result.success);
        assert(result.error == paybridge::transport::TransportError::PLATFORM_ERROR);
        const auto snapshots = testManager.terminalSnapshots();
        assert(snapshots.size() == 1);
        assert(snapshots.front().connectionStatus == "FAILED");
    }

    // A send exception must report uncertain delivery.
    {
        TerminalManager testManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-SEND-EXCEPTION";
        terminal.transportKind = TransportKind::SIMULATED;
        assert(testManager.registerTerminal(terminal));

        auto transport = std::make_unique<ThrowingIoTransport>(true, false);
        auto* transportPtr = transport.get();
        testManager.setTransport(terminal.terminalId, std::move(transport));

        bool escaped = false;
        paybridge::transport::TransportBytes result;
        try {
            result = testManager.sendAndReceive(terminal.terminalId, {0x01}, 16);
        }
        catch (...) { escaped = true; }

        assert(!escaped);
        assert(!result.result.success);
        assert(result.result.error == paybridge::transport::TransportError::PLATFORM_ERROR);
        assert(result.result.message.find("delivery may be uncertain") != std::string::npos);
        assert(transportPtr->sendCalls == 1);
        const auto snapshots = testManager.terminalSnapshots();
        assert(snapshots.size() == 1);
        assert(snapshots.front().connectionStatus == "FAILED");
    }

    // A receive exception after send must report an unresolved outcome.
    {
        TerminalManager testManager;
        TerminalModel terminal;
        terminal.terminalId = "SIM-RECEIVE-EXCEPTION";
        terminal.transportKind = TransportKind::SIMULATED;
        assert(testManager.registerTerminal(terminal));

        auto transport = std::make_unique<ThrowingIoTransport>(false, true);
        auto* transportPtr = transport.get();
        testManager.setTransport(terminal.terminalId, std::move(transport));

        bool escaped = false;
        paybridge::transport::TransportBytes result;
        try {
            result = testManager.sendAndReceive(terminal.terminalId, {0x01}, 16);
        }
        catch (...) { escaped = true; }

        assert(!escaped);
        assert(!result.result.success);
        assert(result.result.error == paybridge::transport::TransportError::PLATFORM_ERROR);
        assert(result.result.message.find("operation outcome is unresolved") != std::string::npos);
        assert(transportPtr->sendCalls == 1);
        const auto snapshots = testManager.terminalSnapshots();
        assert(snapshots.size() == 1);
        assert(snapshots.front().connectionStatus == "FAILED");
    }
TerminalManager manager;
    model.transportKind = TransportKind::SIMULATED;
    model.adapterId = SimulatedTerminalAdapter::ID;
    model.terminalId = request.terminalId;
    manager.registerAdapter(std::make_shared<SimulatedTerminalAdapter>());
    assert(manager.registerTerminal(model));
    manager.setTransport(request.terminalId,
        std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
    std::string selected, selectionError;
    assert(manager.selectTerminal(request.terminalId, selected, selectionError));
    assert(selected == request.terminalId);
    model.terminalId = "SIM-OTHER-002";
    model.adapterId = "missing.adapter";
    assert(!manager.registerTerminal(model));
    assert(!manager.selectTerminal(model.terminalId, selected, selectionError));
    assert(!manager.registerAdapter(std::make_shared<SimulatedTerminalAdapter>()));

    TerminalManager lockingManager;
    TerminalModel serializedTerminal;
    serializedTerminal.terminalId = "SIM-LOCK-001";
    serializedTerminal.adapterId = "test.serialized.v1";
    serializedTerminal.transportKind = TransportKind::SIMULATED;
    std::atomic<int> active{0}, maximum{0};
    lockingManager.registerAdapter(std::make_shared<SlowTestAdapter>(active, maximum));
    assert(lockingManager.registerTerminal(serializedTerminal));
    lockingManager.setTransport(serializedTerminal.terminalId,
        std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
    TerminalOperationRequest first = request;
    first.terminalId = serializedTerminal.terminalId;
    TerminalOperationRequest second = first;
    second.requestId = "1123456789abcdef0123456789abcdef";
    std::atomic<bool> start{false};
    TerminalOperationResult firstResult, secondResult;
    std::thread firstThread([&]
    {
        while (!start.load()) std::this_thread::yield();
        firstResult = lockingManager.executeOperation(first, [] { return true; });
    });
    std::thread secondThread([&]
    {
        while (!start.load()) std::this_thread::yield();
        secondResult = lockingManager.executeOperation(second, [] { return true; });
    });
    start = true;
    firstThread.join();
    secondThread.join();
    assert(firstResult.outcome == TerminalOperationOutcome::APPROVED);
    assert(secondResult.outcome == TerminalOperationOutcome::APPROVED);
    assert(maximum == 1);

    TerminalManager parallelManager;
    TerminalModel parallelOne = serializedTerminal;
    TerminalModel parallelTwo = serializedTerminal;
    parallelOne.terminalId = "SIM-PARALLEL-001";
    parallelTwo.terminalId = "SIM-PARALLEL-002";
    std::atomic<int> parallelActive{0}, parallelMaximum{0};
    parallelManager.registerAdapter(std::make_shared<SlowTestAdapter>(
        parallelActive, parallelMaximum));
    assert(parallelManager.registerTerminal(parallelOne));
    assert(parallelManager.registerTerminal(parallelTwo));
    parallelManager.setTransport(parallelOne.terminalId,
        std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
    parallelManager.setTransport(parallelTwo.terminalId,
        std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
    TerminalOperationRequest parallelRequestOne = request;
    parallelRequestOne.terminalId = parallelOne.terminalId;
    TerminalOperationRequest parallelRequestTwo = request;
    parallelRequestTwo.terminalId = parallelTwo.terminalId;
    parallelRequestTwo.requestId = "2123456789abcdef0123456789abcdef";
    start = false;
    std::thread parallelThreadOne([&]
    {
        while (!start.load()) std::this_thread::yield();
        parallelManager.executeOperation(parallelRequestOne, [] { return true; });
    });
    std::thread parallelThreadTwo([&]
    {
        while (!start.load()) std::this_thread::yield();
        parallelManager.executeOperation(parallelRequestTwo, [] { return true; });
    });
    start = true;
    parallelThreadOne.join();
    parallelThreadTwo.join();
    assert(parallelMaximum == 2);

    // Adapter ambiguity is rejected instead of selecting by unordered-map order.
    TerminalManager ambiguousManager;
    TerminalModel ambiguous = serializedTerminal;
    ambiguous.terminalId = "SIM-AMBIGUOUS-001";
    ambiguous.adapterId.clear();
    ambiguousManager.registerTerminal(ambiguous);
    std::atomic<int> ambiguousActive{0}, ambiguousMaximum{0};
    assert(ambiguousManager.registerAdapter(std::make_shared<SlowTestAdapter>(
        ambiguousActive, ambiguousMaximum, "test.adapter.a")));
    assert(ambiguousManager.registerAdapter(std::make_shared<SlowTestAdapter>(
        ambiguousActive, ambiguousMaximum, "test.adapter.b")));
    ambiguousManager.setTransport(ambiguous.terminalId,
        std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
    assert(!ambiguousManager.selectTerminal({}, selected, selectionError));

    // Lifecycle changes are coordinated with a deterministic operation gate.
    TerminalManager lifecycleManager;
    TerminalModel lifecycleTerminal = serializedTerminal;
    lifecycleTerminal.terminalId = "SIM-LIFECYCLE-001";
    lifecycleTerminal.adapterId = "test.blocking.v1";

    auto blockingAdapter = std::make_shared<BlockingTestAdapter>();
    assert(lifecycleManager.registerAdapter(blockingAdapter));
    assert(lifecycleManager.registerTerminal(lifecycleTerminal));

    lifecycleManager.setTransport(lifecycleTerminal.terminalId,
        std::make_unique<SimulatedTerminalTransport>(
            SimulatedTerminalOutcome::APPROVED));

    TerminalOperationRequest lifecycleRequest = request;
    lifecycleRequest.terminalId = lifecycleTerminal.terminalId;

    TerminalOperationResult lifecycleResult;
    std::thread activeOperation([&]
    {
        lifecycleResult = lifecycleManager.executeOperation(
            lifecycleRequest, [] { return true; });
    });

    if (!blockingAdapter->waitUntilEntered())
    {
        blockingAdapter->release();
        activeOperation.join();
        assert(false && "Lifecycle operation did not enter adapter");
    }

    std::atomic<bool> reconfigurationDone{false};
    std::atomic<bool> transportReplacementDone{false};

    std::thread reconfigure([&]
    {
        auto updated = lifecycleTerminal;
        updated.description = "updated after active operation";
        lifecycleManager.registerTerminal(updated);
        reconfigurationDone = true;
    });

    std::thread replaceTransport([&]
    {
        lifecycleManager.setTransport(lifecycleTerminal.terminalId,
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::DECLINED));
        transportReplacementDone = true;
    });

    blockingAdapter->release();

    activeOperation.join();
    reconfigure.join();
    replaceTransport.join();

    assert(lifecycleResult.outcome == TerminalOperationOutcome::APPROVED);
    assert(reconfigurationDone.load());
    assert(transportReplacementDone.load());

    // Verify the replacement transport is usable after the active operation.
    TerminalOperationRequest replacementRequest = lifecycleRequest;
    replacementRequest.requestId = "3123456789abcdef0123456789abcdef";

    const auto replacementResult = lifecycleManager.executeOperation(
        replacementRequest, [] { return true; });

    assert(replacementResult.outcome == TerminalOperationOutcome::DECLINED);

    std::cout << "Generic terminal operation contracts passed\n";
    return 0;
}


