#pragma once

#include "../assets/Asset.h"
#include "../assets/AssetRegistry.h"
#include "../money/Money.h"

#include <string>
#include <nlohmann/json.hpp>

struct PaymentRequest
{
    std::string externalReference;
    std::string transactionId;
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

    static bool validateMessage(const std::string& message);

    static nlohmann::json parseMessage(const std::string& message);

    static PaymentValidation parseAndValidatePaymentRequest(
        const std::string& message,
        paybridge::assets::AssetRegistry& registry
    );
};
