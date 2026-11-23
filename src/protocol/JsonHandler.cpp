#include "JsonHandler.h"

using json = nlohmann::json;

std::string JsonHandler::createPaymentRequest(
    const std::string& transactionId,
    long long amountMinor,
    const std::string& currency)
{
    json request;

    request["protocol_version"] = "1.0";
    request["message_type"] = "PAYMENT_REQUEST";
    request["transaction_id"] = transactionId;

    request["payment"] = {
        {"amount_minor", amountMinor},
        {"currency", currency}
    };

    request["metadata"] = {
        {"source", "PayBridge"},
        {"environment", "SIMULATION"}
    };

    return request.dump();
}

bool JsonHandler::validateMessage(const std::string& message)
{
    try
    {
        json data = json::parse(message);

        return data.is_object()
            && data.contains("message_type")
            && data["message_type"].is_string()
            && data.contains("transaction_id")
            && data["transaction_id"].is_string();
    }
    catch (...)
    {
        return false;
    }
}

json JsonHandler::parseMessage(const std::string& message)
{
    return json::parse(message);
}

