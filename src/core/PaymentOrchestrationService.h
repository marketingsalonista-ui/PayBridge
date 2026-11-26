#pragma once

#include "TransactionEngine.h"
#include "../protocol/JsonHandler.h"
#include "../terminal/TerminalManager.h"

#include <nlohmann/json_fwd.hpp>

struct PaymentOrchestrationResult
{
    std::string status;
    std::string errorCode;
    std::string message;
    std::string terminalId;
    std::string resultCode;
    std::string transactionId;
    std::string externalReference;
    bool simulated = true;
};

class PaymentOrchestrationService
{
public:
    PaymentOrchestrationService(TransactionEngine& engine,
        paybridge::terminal::TerminalManager& terminals);

    PaymentOrchestrationResult process(
        const PaymentRequest& request,
        const std::string& requestedTerminalId = {});

private:
    TransactionEngine& engine;
    paybridge::terminal::TerminalManager& terminals;
};
