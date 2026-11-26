#pragma once

#include "../assets/Asset.h"
#include "../money/Money.h"

#include <string>

namespace paybridge::terminal
{

enum class TerminalOperationKind
{
    PAYMENT
};

enum class TerminalOperationOutcome
{
    APPROVED,
    DECLINED,
    FAILED,
    UNKNOWN
};

// Vendor-neutral information needed to request one terminal operation.
// Amounts remain exact integer units at the asset's configured precision.
struct TerminalOperationRequest
{
    std::string requestId;
    std::string transactionId;
    std::string externalReference;
    std::string terminalId;
    TerminalOperationKind kind = TerminalOperationKind::PAYMENT;
    paybridge::money::Money amount;
    paybridge::assets::Asset asset;

    bool isValid() const
    {
        return !requestId.empty() && !transactionId.empty() &&
            !externalReference.empty() && !terminalId.empty() &&
            kind == TerminalOperationKind::PAYMENT && amount.isPositive() &&
            asset.isComplete() && amount.precision == asset.decimalPrecision;
    }
};

// Normalized adapter result. Adapters must copy the four correlation fields
// from the operation they handled. Raw protocol bytes do not belong here.
struct TerminalOperationResult
{
    TerminalOperationOutcome outcome = TerminalOperationOutcome::FAILED;
    std::string requestId;
    std::string transactionId;
    std::string externalReference;
    std::string terminalId;
    std::string resultCode;
    std::string message;
    bool simulated = false;
    bool requestMayHaveBeenSent = false;

    bool correlatesTo(const TerminalOperationRequest& request) const
    {
        return request.isValid() && requestId == request.requestId &&
            transactionId == request.transactionId &&
            externalReference == request.externalReference &&
            terminalId == request.terminalId;
    }
};

} // namespace paybridge::terminal
