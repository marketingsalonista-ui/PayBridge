#include "SimulatedTerminalProtocol.h"

#include <nlohmann/json.hpp>

namespace paybridge::terminal
{
namespace
{
using Json = nlohmann::json;
constexpr const char* CONTRACT = "PAYBRIDGE_SIMULATED_TERMINAL_V1";

bool hasIdentity(const SimulatedTerminalRequest& request)
{
    return !request.requestId.empty() && !request.transactionId.empty() &&
        !request.externalReference.empty() && !request.terminalId.empty() &&
        !request.amount.empty() && !request.assetIdentity.empty();
}
}

std::vector<std::uint8_t> SimulatedTerminalProtocol::encodeRequest(
    const SimulatedTerminalRequest& request)
{
    if (!hasIdentity(request)) return {};
    const Json data = {
        {"simulation_contract", CONTRACT}, {"simulated", true},
        {"request_id", request.requestId}, {"transaction_id", request.transactionId},
        {"external_reference", request.externalReference}, {"terminal_id", request.terminalId},
        {"amount", request.amount}, {"asset_identity", request.assetIdentity}
    };
    const std::string text = data.dump();
    return {text.begin(), text.end()};
}

std::optional<SimulatedTerminalRequest> SimulatedTerminalProtocol::decodeRequest(
    const std::vector<std::uint8_t>& payload)
{
    if (payload.empty() || payload.size() > 65536) return std::nullopt;
    try
    {
        const Json data = Json::parse(payload.begin(), payload.end());
        if (!data.is_object() ||
            data.value("simulation_contract", std::string{}) != CONTRACT ||
            !data.value("simulated", false)) return std::nullopt;

        SimulatedTerminalRequest request;
        request.requestId = data.at("request_id").get<std::string>();
        request.transactionId = data.at("transaction_id").get<std::string>();
        request.externalReference = data.at("external_reference").get<std::string>();
        request.terminalId = data.at("terminal_id").get<std::string>();
        request.amount = data.at("amount").get<std::string>();
        request.assetIdentity = data.at("asset_identity").get<std::string>();
        if (!hasIdentity(request)) return std::nullopt;
        return request;
    }
    catch (...) { return std::nullopt; }
}

std::vector<std::uint8_t> SimulatedTerminalProtocol::encodeResponse(
    const SimulatedTerminalRequest& request,
    SimulatedTerminalOutcome outcome)
{
    if (!hasIdentity(request) ||
        (outcome != SimulatedTerminalOutcome::APPROVED &&
         outcome != SimulatedTerminalOutcome::DECLINED))
        return {};
    const char* outcomeText = outcome == SimulatedTerminalOutcome::APPROVED
        ? "APPROVED" : "DECLINED";
    const Json data = {
        {"simulation_contract", CONTRACT}, {"simulated", true},
        {"outcome", outcomeText}, {"request_id", request.requestId},
        {"transaction_id", request.transactionId},
        {"external_reference", request.externalReference},
        {"terminal_id", request.terminalId}
    };
    const std::string text = data.dump();
    const std::vector<std::uint8_t> payload(text.begin(), text.end());
    return framePayload(payload);
}

std::vector<std::uint8_t> SimulatedTerminalProtocol::framePayload(
    const std::vector<std::uint8_t>& payload)
{
    if (payload.empty() || payload.size() > 65536) return {};

    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame;
    frame.reserve(4 + payload.size());
    frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(length & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::optional<std::size_t> SimulatedTerminalProtocol::decodeFrameLength(
    const std::vector<std::uint8_t>& header)
{
    if (header.size() != 4) return std::nullopt;

    const std::uint32_t length =
        (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) |
        static_cast<std::uint32_t>(header[3]);

    if (length == 0 || length > 65536) return std::nullopt;
    return static_cast<std::size_t>(length);
}

bool SimulatedTerminalProtocol::validateRequest(
    const SimulatedTerminalRequest& request,
    const std::vector<std::uint8_t>& payload) const
{
    return !payload.empty() && payload.size() <= 65536 &&
        encodeRequest(request) == payload;
}

std::optional<SimulatedTerminalResponse> SimulatedTerminalProtocol::decodeResponse(
    const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < 5 || payload.size() > 65540) return std::nullopt;

    const std::vector<std::uint8_t> header(payload.begin(), payload.begin() + 4);
    const auto frameLength = decodeFrameLength(header);
    if (!frameLength || payload.size() != 4 + *frameLength) return std::nullopt;

    const auto bodyBegin = payload.begin() + 4;
    try
    {
        const Json data = Json::parse(bodyBegin, payload.end());
        if (!data.is_object() || data.value("simulation_contract", std::string{}) != CONTRACT ||
            !data.value("simulated", false)) return std::nullopt;

        SimulatedTerminalResponse response;
        const std::string outcome = data.at("outcome").get<std::string>();
        if (outcome == "APPROVED") response.outcome = SimulatedTerminalOutcome::APPROVED;
        else if (outcome == "DECLINED") response.outcome = SimulatedTerminalOutcome::DECLINED;
        else return std::nullopt;
        response.requestId = data.at("request_id").get<std::string>();
        response.transactionId = data.at("transaction_id").get<std::string>();
        response.externalReference = data.at("external_reference").get<std::string>();
        response.terminalId = data.at("terminal_id").get<std::string>();
        if (response.requestId.empty() || response.transactionId.empty() ||
            response.externalReference.empty() || response.terminalId.empty()) return std::nullopt;
        response.simulatedResult = outcome == "APPROVED"
            ? "SIMULATED APPROVED - no real payment was processed"
            : "SIMULATED DECLINED - no real payment was processed";
        return response;
    }
    catch (...) { return std::nullopt; }
}

bool SimulatedTerminalProtocol::validateResponse(
    const std::vector<std::uint8_t>& payload,
    const SimulatedTerminalRequest& expectedRequest) const
{
    const auto response = decodeResponse(payload);
    return response && response->requestId == expectedRequest.requestId &&
        response->transactionId == expectedRequest.transactionId &&
        response->externalReference == expectedRequest.externalReference &&
        response->terminalId == expectedRequest.terminalId;
}

SimulatedTerminalResponse SimulatedTerminalProtocol::parseResponse(
    const std::vector<std::uint8_t>& payload,
    const SimulatedTerminalRequest& expectedRequest)
{
    const auto response = decodeResponse(payload);
    if (!response || response->requestId != expectedRequest.requestId ||
        response->transactionId != expectedRequest.transactionId ||
        response->externalReference != expectedRequest.externalReference ||
        response->terminalId != expectedRequest.terminalId)
    {
        SimulatedTerminalResponse malformed;
        malformed.outcome = SimulatedTerminalOutcome::MALFORMED;
        malformed.simulatedResult = "INVALID OR UNCORRELATED SIMULATED terminal response";
        return malformed;
    }
    return *response;
}

} // namespace paybridge::terminal
