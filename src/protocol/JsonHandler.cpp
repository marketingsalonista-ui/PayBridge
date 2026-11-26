#include "JsonHandler.h"

#include "../money/AmountParser.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <limits>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sstream>

using json = nlohmann::json;

namespace
{
constexpr std::int64_t MAX_REQUEST_LIFETIME_SECONDS = 300;
constexpr std::int64_t MAX_CLOCK_SKEW_SECONDS = 300;

std::int64_t currentUnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool isHexRequestId(const std::string& requestId)
{
    if (requestId.size() != 32)
        return false;

    for (unsigned char character : requestId)
    {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'A' && character <= 'F') ||
              (character >= 'a' && character <= 'f')))
            return false;
    }
    return true;
}

bool getUnixTimestamp(const json& data, const char* fieldName, std::int64_t& value)
{
    if (!data.contains(fieldName))
        return false;

    const json& timestamp = data[fieldName];

    if (timestamp.is_number_unsigned())
    {
        const std::uint64_t unsignedValue = timestamp.get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return false;

        value = static_cast<std::int64_t>(unsignedValue);
        return true;
    }

    if (timestamp.is_number_integer())
    {
        value = timestamp.get<std::int64_t>();
        return true;
    }

    return false;
}

std::string sha256Hex(const std::string& value)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes.data());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}
}

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

std::string JsonHandler::createPaymentRequest(
    const std::string& transactionId,
    const std::string& externalReference,
    const paybridge::assets::Asset& asset,
    const paybridge::money::Money& money,
    const std::string& requestId,
    std::int64_t requestTimestamp,
    std::int64_t requestExpiry)
{
    json request;

    request["protocol_version"] = "1.0";
    request["message_type"] = "PAYMENT_REQUEST";
    request["transaction_id"] = transactionId;
    request["request_id"] = requestId;
    request["request_timestamp"] = requestTimestamp;
    request["request_expiry"] = requestExpiry;
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
            && data.contains("request_id")
            && data["request_id"].is_string()
            && data.contains("request_timestamp")
            && (data["request_timestamp"].is_number_integer() || data["request_timestamp"].is_number_unsigned())
            && data.contains("request_expiry")
            && (data["request_expiry"].is_number_integer() || data["request_expiry"].is_number_unsigned())
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
        if (data.contains("terminal_id"))
        {
            if (!data["terminal_id"].is_string() ||
                data["terminal_id"].get<std::string>().empty())
            {
                result.errorCode = "INVALID_TERMINAL_ID";
                result.message = "terminal_id must be a non-empty string when supplied";
                return result;
            }
            result.request.terminalId = data["terminal_id"].get<std::string>();
        }

        if (!data.contains("request_id") || !data["request_id"].is_string())
        {
            result.errorCode = "INVALID_REQUEST_ID";
            result.message = "request_id is mandatory";
            return result;
        }

        result.request.envelope.requestId = data["request_id"].get<std::string>();
        if (!isHexRequestId(result.request.envelope.requestId))
        {
            result.errorCode = "INVALID_REQUEST_ID";
            result.message = "request_id must be exactly 32 hexadecimal characters";
            return result;
        }

        if (!getUnixTimestamp(data, "request_timestamp", result.request.envelope.requestTimestamp))
        {
            result.errorCode = "INVALID_REQUEST_TIMESTAMP";
            result.message = "request_timestamp must be a valid Unix timestamp";
            return result;
        }

        if (!getUnixTimestamp(data, "request_expiry", result.request.envelope.requestExpiry))
        {
            result.errorCode = "INVALID_REQUEST_EXPIRY";
            result.message = "request_expiry must be a valid Unix timestamp";
            return result;
        }

        if (result.request.envelope.requestTimestamp < 0 ||
            result.request.envelope.requestExpiry < 0)
        {
            result.errorCode = "INVALID_REQUEST_TIMESTAMP";
            result.message = "Request timestamps must be non-negative Unix timestamps";
            return result;
        }

        if (result.request.envelope.requestExpiry <= result.request.envelope.requestTimestamp)
        {
            result.errorCode = "INVALID_REQUEST_EXPIRY";
            result.message = "request_expiry must be later than request_timestamp";
            return result;
        }

        const bool lifetimeExceeded =
            result.request.envelope.requestExpiry >= MAX_REQUEST_LIFETIME_SECONDS &&
            result.request.envelope.requestTimestamp <
                result.request.envelope.requestExpiry - MAX_REQUEST_LIFETIME_SECONDS;
        if (lifetimeExceeded)
        {
            result.errorCode = "REQUEST_LIFETIME_EXCEEDED";
            result.message = "Request lifetime exceeds the maximum permitted duration";
            return result;
        }

        const std::int64_t now = currentUnixSeconds();
        if (result.request.envelope.requestTimestamp > now &&
            result.request.envelope.requestTimestamp - now > MAX_CLOCK_SKEW_SECONDS)
        {
            result.errorCode = "REQUEST_TIMESTAMP_INVALID";
            result.message = "request_timestamp is unreasonably in the future";
            return result;
        }

        if (now > result.request.envelope.requestTimestamp &&
            now - result.request.envelope.requestTimestamp > MAX_CLOCK_SKEW_SECONDS)
        {
            result.errorCode = "REQUEST_TIMESTAMP_INVALID";
            result.message = "request_timestamp is unreasonably in the past";
            return result;
        }

        if (result.request.envelope.requestExpiry <= now)
        {
            result.errorCode = "REQUEST_EXPIRED";
            result.message = "Request has expired";
            return result;
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
        std::transform(result.request.envelope.requestId.begin(), result.request.envelope.requestId.end(),
                       result.request.envelope.requestId.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        json fingerprint = {
            {"external_reference", result.request.externalReference},
            {"terminal_id", result.request.terminalId},
            {"amount_units", parsed.money.units}, {"precision", parsed.money.precision},
            {"asset_type", paybridge::assets::assetTypeToString(asset.type)}, {"asset_code", asset.code},
            {"network", asset.network}, {"contract_address", asset.contractAddress},
            {"currency", asset.isFiat() ? asset.code : std::string{}}
        };
        result.request.paymentFingerprint = sha256Hex(fingerprint.dump());
        json canonical = {
            {"protocol_version", "1.0"}, {"message_type", "PAYMENT_REQUEST"},
            {"request_id", result.request.envelope.requestId},
            {"request_timestamp", result.request.envelope.requestTimestamp},
            {"request_expiry", result.request.envelope.requestExpiry},
            {"payment_fingerprint", result.request.paymentFingerprint}
        };
        result.request.canonicalRequestHash = sha256Hex(canonical.dump());
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

std::string JsonHandler::generateRequestId()
{
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        return {};

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : random)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}
