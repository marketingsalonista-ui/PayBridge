#include "PaymentOrchestrationService.h"

using namespace paybridge::terminal;

namespace
{
PaymentOrchestrationResult resultFor(const Transaction& transaction,
    const std::string& status, const std::string& code, const std::string& message,
    const std::string& terminalId = {})
{
    return {status, code, message, terminalId, code,
        transaction.transactionId, transaction.externalReference, true};
}
}

PaymentOrchestrationService::PaymentOrchestrationService(
    TransactionEngine& engine, TerminalManager& terminals)
    : engine(engine), terminals(terminals)
{
}

PaymentOrchestrationResult PaymentOrchestrationService::process(
    const PaymentRequest& request, const std::string& requestedTerminalId)
{
    RequestReplayData replay;
    replay.requestId = request.envelope.requestId;
    replay.canonicalRequestHash = request.canonicalRequestHash;
    replay.paymentFingerprint = request.paymentFingerprint;
    replay.requestTimestamp = request.envelope.requestTimestamp;
    replay.requestExpiry = request.envelope.requestExpiry;

    auto registration = engine.createTransaction(request.money, request.asset,
        request.externalReference, replay);
    if (registration.code == ReplayRegistrationCode::REQUEST_ID_CONFLICT)
        return resultFor(registration.transaction, "REJECTED", "REPLAY_CONFLICT",
            "request_id was previously used for a different payment");
    if (registration.code == ReplayRegistrationCode::EXTERNAL_REFERENCE_CONFLICT)
        return resultFor(registration.transaction, "REJECTED", "EXTERNAL_REFERENCE_CONFLICT",
            "external_reference belongs to a different payment");
    if (registration.code == ReplayRegistrationCode::DATABASE_ERROR)
        return resultFor(registration.transaction, "REJECTED", "PERSISTENCE_FAILURE",
            "Request could not be persisted");

    Transaction transaction = registration.transaction;
    if (!registration.shouldProcess)
    {
        const std::string status = registration.replayState == ReplayState::PROCESSING
            ? "PROCESSING" : registration.replayState == ReplayState::UNKNOWN
            ? "UNKNOWN" : registration.responseStatus.empty()
            ? TransactionEngine::statusToString(transaction.status) : registration.responseStatus;
        return resultFor(transaction, status, registration.adapterResultCode,
            registration.adapterMessage.empty() ? "Previously recorded simulated payment operation"
                                                : registration.adapterMessage,
            registration.terminalId);
    }

    std::string terminalId, selectionError;
    if (!terminals.selectTerminal(requestedTerminalId, terminalId, selectionError))
    {
        engine.updateStatusForRequest(transaction, TransactionStatus::FAILED,
            replay.requestId, ReplayState::FAILED, "FAILED", "TERMINAL_UNAVAILABLE", selectionError);
        return resultFor(transaction, "FAILED", "TERMINAL_UNAVAILABLE", selectionError);
    }
    if (!engine.bindSimulatedTerminal(replay.requestId, terminalId,
        "PAYBRIDGE_SIMULATED_TERMINAL_V1"))
    {
        return resultFor(transaction, "REJECTED", "PERSISTENCE_FAILURE",
            "Simulated terminal assignment could not be persisted", terminalId);
    }

    TerminalOperationRequest operation;
    operation.requestId = replay.requestId;
    operation.transactionId = transaction.transactionId;
    operation.externalReference = transaction.externalReference;
    operation.terminalId = terminalId;
    operation.kind = TerminalOperationKind::PAYMENT;
    operation.amount = transaction.money;
    operation.asset = transaction.asset;

    // The terminal has already been contacted when this runs. If its outcome
    // cannot be recorded, report UNKNOWN rather than REJECTED so that no caller
    // treats the payment as safely retryable. Recording UNKNOWN is best effort:
    // the response is UNKNOWN either way, and a replay never resends.
    const auto unresolvedAfterSend = [&](const char* detail)
    {
        engine.updateStatusForRequest(transaction, TransactionStatus::UNKNOWN,
            replay.requestId, ReplayState::UNKNOWN, "UNKNOWN",
            "PERSISTENCE_FAILURE_AFTER_SEND", detail,
            "Terminal outcome could not be persisted; automatic resend is disabled");
        return resultFor(transaction, "UNKNOWN", "PERSISTENCE_FAILURE_AFTER_SEND",
            "SIMULATED result is unresolved; automatic resend is disabled", terminalId);
    };

    bool sentStatePersisted = false;
    const auto terminalResult = terminals.executeOperation(operation, [&]()
    {
        sentStatePersisted = engine.updateStatusForRequest(transaction,
            TransactionStatus::SENT, replay.requestId, ReplayState::PROCESSING,
            "SENT", "SIMULATED_REQUEST_SENT",
            "SIMULATED request sent; no real payment was processed");
        return sentStatePersisted;
    });
    if (!sentStatePersisted && terminalResult.resultCode == "PERSISTENCE_FAILURE")
        return resultFor(transaction, "REJECTED", "PERSISTENCE_FAILURE",
            "Transaction state could not be persisted before terminal I/O", terminalId);
    if (!terminalResult.correlatesTo(operation))
    {
        if (terminalResult.requestMayHaveBeenSent)
        {
            engine.updateStatusForRequest(transaction, TransactionStatus::UNKNOWN,
                replay.requestId, ReplayState::UNKNOWN, "UNKNOWN", "ADAPTER_RESPONSE_UNCORRELATED",
                "Adapter response correlation failed; automatic resend is disabled");
            return resultFor(transaction, "UNKNOWN", "ADAPTER_RESPONSE_UNCORRELATED",
                "SIMULATED result is unresolved; automatic resend is disabled", terminalId);
        }
        engine.updateStatusForRequest(transaction, TransactionStatus::FAILED,
            replay.requestId, ReplayState::FAILED, "FAILED", terminalResult.resultCode,
            terminalResult.message);
        return resultFor(transaction, "FAILED", terminalResult.resultCode,
            terminalResult.message, terminalId);
    }
    if (terminalResult.outcome == TerminalOperationOutcome::FAILED)
    {
        engine.updateStatusForRequest(transaction, TransactionStatus::FAILED,
            replay.requestId, ReplayState::FAILED, "FAILED", terminalResult.resultCode,
            terminalResult.message);
        return resultFor(transaction, "FAILED", terminalResult.resultCode,
            terminalResult.message, terminalId);
    }
    if (terminalResult.outcome == TerminalOperationOutcome::UNKNOWN)
    {
        engine.updateStatusForRequest(transaction, TransactionStatus::UNKNOWN,
            replay.requestId, ReplayState::UNKNOWN, "UNKNOWN", terminalResult.resultCode,
            terminalResult.message, "No correlated terminal outcome was received");
        return resultFor(transaction, "UNKNOWN", terminalResult.resultCode,
            "SIMULATED result is unresolved; automatic resend is disabled", terminalId);
    }

    if (!engine.updateStatusForRequest(transaction, TransactionStatus::RECEIVED,
        replay.requestId, ReplayState::PROCESSING, "RECEIVED", "SIMULATED_RESPONSE_RECEIVED",
        "Correlated simulated response received; no real payment was processed"))
        return unresolvedAfterSend(
            "Simulated response received but receipt state could not be persisted");

    const bool approved = terminalResult.outcome == TerminalOperationOutcome::APPROVED;
    const auto finalStatus = approved ? TransactionStatus::APPROVED : TransactionStatus::DECLINED;
    const std::string finalStatusText = approved ? "APPROVED" : "DECLINED";
    const std::string resultCode = approved ? "SIMULATED_APPROVED" : "SIMULATED_DECLINED";
    if (!engine.updateStatusForRequest(transaction, finalStatus, replay.requestId,
        ReplayState::COMPLETED, finalStatusText, resultCode, terminalResult.message))
        return unresolvedAfterSend("Simulated outcome could not be persisted");
    return resultFor(transaction, finalStatusText, resultCode,
        terminalResult.message, terminalId);
}
