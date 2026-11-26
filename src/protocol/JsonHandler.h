#pragma once

#include "../assets/Asset.h"
#include "../assets/AssetRegistry.h"
#include "../money/Money.h"

#include <string>
#include <nlohmann/json.hpp>
#include <cstdint>

struct RequestEnvelope
{
    std::string requestId;
    std::int64_t requestTimestamp = 0;
    std::int64_t requestExpiry = 0;
};

struct PaymentRequest
{
    RequestEnvelope envelope;
    std::string terminalId;
    std::string externalReference;
    std::string transactionId;
    std::string canonicalRequestHash;
    std::string paymentFingerprint;
    paybridge::assets::Asset asset;
    paybridge::money::Money money;
};

struct PaymentValidation
{
    bool valid = false;
    std::string errorCode;
    std::string message;
    PaymentRequest request;
};

class JsonHandler
{
public:
    static std::string createPaymentRequest(
        const std::string& transactionId,
        long long amountMinor,
        const std::string& currency
    );

    static std::string createPaymentRequest(
        const std::string& transactionId,
        const std::string& externalReference,
        const paybridge::assets::Asset& asset,
        const paybridge::money::Money& money
    );

    static std::string createPaymentRequest(
        const std::string& transactionId,
        const std::string& externalReference,
        const paybridge::assets::Asset& asset,
        const paybridge::money::Money& money,
        const std::string& requestId,
        std::int64_t requestTimestamp,
        std::int64_t requestExpiry
    );

    static bool validateMessage(const std::string& message);

    static nlohmann::json parseMessage(const std::string& message);

    static PaymentValidation parseAndValidatePaymentRequest(
        const std::string& message,
        paybridge::assets::AssetRegistry& registry
    );

    static std::string generateRequestId();
};
