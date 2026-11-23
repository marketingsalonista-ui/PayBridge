#pragma once

#include <string>
#include <nlohmann/json.hpp>

class JsonHandler
{
public:
    static std::string createPaymentRequest(
        const std::string& transactionId,
        long long amountMinor,
        const std::string& currency
    );

    static bool validateMessage(const std::string& message);

    static nlohmann::json parseMessage(const std::string& message);
};
