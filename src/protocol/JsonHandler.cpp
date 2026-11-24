#include "JsonHandler.h"

#include "../money/AmountParser.h"

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

std::string JsonHandler::createPaymentRequest(
    const std::string& transactionId,
    const std::string& externalReference,
    const paybridge::assets::Asset& asset,
    const paybridge::money::Money& money)
{
    json request;

    request["protocol_version"] = "1.0";
    request["message_type"] = "PAYMENT_REQUEST";
    request["transaction_id"] = transactionId;
    request["external_reference"] = externalReference;

    json assetJson;
    assetJson["type"] = paybridge::assets::assetTypeToString(asset.type);
    assetJson["code"] = asset.code;

    if (asset.isDigitalAsset())
    {
        assetJson["network"] = asset.network;
        assetJson["contract_address"] = asset.contractAddress;
    }

    request["payment"] = {
        {"amount", paybridge::money::formatAmount(money)},
        {"asset", assetJson}
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
            && data.contains("protocol_version")
            && data["protocol_version"].is_string()
            && data["protocol_version"] == "1.0"
            && data.contains("message_type")
            && data["message_type"].is_string()
            && data.contains("external_reference")
            && data["external_reference"].is_string()
            && data.contains("payment")
            && data["payment"].is_object();
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

PaymentValidation JsonHandler::parseAndValidatePaymentRequest(
    const std::string& message,
    paybridge::assets::AssetRegistry& registry)
{
    PaymentValidation result;

    try
    {
        const json data = json::parse(message);

        if (!data.is_object())
        {
            result.errorCode = "INVALID_MESSAGE";
            result.message = "Message must be a JSON object";
            return result;
        }

        if (!data.contains("protocol_version") ||
            !data["protocol_version"].is_string() ||
            data["protocol_version"].get<std::string>() != "1.0")
        {
            result.errorCode = "UNSUPPORTED_PROTOCOL";
            result.message = "protocol_version must be 1.0";
            return result;
        }

        if (!data.contains("message_type") ||
            !data["message_type"].is_string() ||
            data["message_type"].get<std::string>() != "PAYMENT_REQUEST")
        {
            result.errorCode = "INVALID_MESSAGE_TYPE";
            result.message = "message_type must be PAYMENT_REQUEST";
            return result;
        }

        if (!data.contains("external_reference") ||
            !data["external_reference"].is_string() ||
            data["external_reference"].get<std::string>().empty())
        {
            result.errorCode = "INVALID_EXTERNAL_REFERENCE";
            result.message = "external_reference is mandatory and non-empty";
            return result;
        }

        result.request.externalReference =
            data["external_reference"].get<std::string>();

        if (data.contains("transaction_id"))
        {
            if (!data["transaction_id"].is_string() ||
                data["transaction_id"].get<std::string>().empty())
            {
                result.errorCode = "INVALID_TRANSACTION_ID";
                result.message = "transaction_id must be non-empty when provided";
                return result;
            }

            result.request.transactionId =
                data["transaction_id"].get<std::string>();
        }

        if (!data.contains("payment") || !data["payment"].is_object())
        {
            result.errorCode = "INVALID_PAYMENT";
            result.message = "payment must be a JSON object";
            return result;
        }

        const json& payment = data["payment"];
        if (!payment.contains("amount") || !payment["amount"].is_string())
        {
            result.errorCode = "INVALID_AMOUNT";
            result.message = "payment.amount must be a decimal string";
            return result;
        }

        if (!payment.contains("asset") || !payment["asset"].is_object())
        {
            result.errorCode = "INVALID_ASSET";
            result.message = "payment.asset must be a JSON object";
            return result;
        }

        const json& assetJson = payment["asset"];
        if (!assetJson.contains("type") || !assetJson["type"].is_string() ||
            !assetJson.contains("code") || !assetJson["code"].is_string())
        {
            result.errorCode = "INVALID_ASSET";
            result.message = "asset.type and asset.code are mandatory";
            return result;
        }

        paybridge::assets::Asset asset;
        asset.code = assetJson["code"].get<std::string>();

        const std::string type = assetJson["type"].get<std::string>();

        if (type == "FIAT")
        {
            asset.type = paybridge::assets::AssetType::FIAT;
        }
        else if (type == "DIGITAL_ASSET")
        {
            asset.type = paybridge::assets::AssetType::DIGITAL_ASSET;

            if (!assetJson.contains("network") || !assetJson["network"].is_string() ||
                !assetJson.contains("contract_address") || !assetJson["contract_address"].is_string())
            {
                result.errorCode = "INVALID_ASSET_IDENTITY";
                result.message = "Digital assets require network and contract_address";
                return result;
            }

            asset.network = assetJson["network"].get<std::string>();
            asset.contractAddress = assetJson["contract_address"].get<std::string>();
        }
        else
        {
            result.errorCode = "UNSUPPORTED_ASSET_TYPE";
            result.message = "Asset type must be FIAT or DIGITAL_ASSET";
            return result;
        }

        std::optional<paybridge::assets::Asset> resolved;

        if (asset.type == paybridge::assets::AssetType::FIAT)
            resolved = registry.findFiat(asset.code);
        else
            resolved = registry.resolveDigitalAsset(asset.code, asset.network, asset.contractAddress);

        if (!resolved)
        {
            result.errorCode = "UNSUPPORTED_ASSET";
            result.message = "Asset is not supported or has untrusted identity";
            return result;
        }

        asset = *resolved;

        const std::string amount = payment["amount"].get<std::string>();
        const auto parsed = paybridge::money::parseAmount(
            amount,
            asset.decimalPrecision
        );

        if (!parsed.success())
        {
            result.errorCode = "INVALID_AMOUNT";
            result.message = "Invalid decimal amount";
            return result;
        }

        result.request.asset = asset;
        result.request.money = parsed.money;
        result.valid = true;
        return result;
    }
    catch (const json::exception&)
    {
        result.errorCode = "INVALID_JSON";
        result.message = "Malformed JSON message";
        return result;
    }
    catch (...)
    {
        result.errorCode = "INVALID_MESSAGE";
        result.message = "Unable to parse payment request";
        return result;
    }
}

