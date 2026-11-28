#include "../src/communication/DashboardApiServer.h"
#include "../src/core/TransactionEngine.h"
#include "../src/security/AuthManager.h"
#include "../src/terminal/SimulatedTerminalTransport.h"
#include "../src/terminal/TerminalManager.h"
#include "../src/terminal/SimulatedTerminalAdapter.h"
#include "../src/terminal/VerifoneVx680SimulatorAdapter.h"
#include "../src/terminal/Vx680UnverifiedAdapter.h"
#include "../src/core/PaymentOrchestrationService.h"
#include "../src/protocol/JsonHandler.h"

#include <winsock2.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct HttpResult
{
    unsigned status = 0;
    std::string body;
};

HttpResult sendRequest(std::uint16_t port, const std::string& target,
    const std::optional<std::string>& authorization = std::nullopt,
    http::verb method = http::verb::get, const std::string& body = {})
{
    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address_v4("127.0.0.1"), port});
    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (authorization) request.set(http::field::authorization, *authorization);
    request.body() = body;
    if (method == http::verb::post || !body.empty()) request.prepare_payload();
    http::write(socket, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return {response.result_int(), response.body()};
}

HttpResult sendRawRequest(std::uint16_t port, const std::string& requestText)
{
    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address_v4("127.0.0.1"), port});
    asio::write(socket, asio::buffer(requestText));
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return {response.result_int(), response.body()};
}

nlohmann::json parsed(const HttpResult& result)
{
    return nlohmann::json::parse(result.body);
}

// Counts send attempts so replay tests can prove a retry never reaches the terminal.
class PostRouteCountingTransport final : public paybridge::terminal::SimulatedTerminalTransport
{
public:
    PostRouteCountingTransport(paybridge::terminal::SimulatedTerminalOutcome outcome,
        std::shared_ptr<std::atomic<int>> counter)
        : SimulatedTerminalTransport(outcome), sends(std::move(counter)) {}

    paybridge::transport::TransportResult send(const std::vector<std::uint8_t>& bytes) override
    {
        ++*sends;
        return SimulatedTerminalTransport::send(bytes);
    }

private:
    std::shared_ptr<std::atomic<int>> sends;
};

int main()
{
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    const auto root = std::filesystem::temp_directory_path() /
        ("paybridge_dashboard_api_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    const auto databasePath = root / "dashboard.sqlite";
    TransactionEngine engine(databasePath.string(), {});
    assert(engine.initialize());
    const auto usd = engine.assets().findFiat("USD");
    assert(usd);
    Transaction first = engine.createTransaction(
        paybridge::money::Money{1234567890123LL, 2}, *usd, "PRIVATE-EXTERNAL-REFERENCE-ONE");
    Transaction second = engine.createTransaction(
        paybridge::money::Money{9999, 2}, *usd, "PRIVATE-EXTERNAL-REFERENCE-TWO");
    assert(first.status == TransactionStatus::CREATED && second.status == TransactionStatus::CREATED);
    assert(engine.updateStatus(second, TransactionStatus::SENT));
    assert(engine.updateStatus(second, TransactionStatus::RECEIVED));
    assert(engine.updateStatus(second, TransactionStatus::APPROVED));
    assert(engine.assets().registerUsdt("PRIVATE-NETWORK", "PRIVATE-CONTRACT-DETAIL", 6));

    paybridge::terminal::TerminalManager terminals;
    assert(terminals.registerAdapter(
        std::make_shared<paybridge::terminal::SimulatedTerminalAdapter>()));
    assert(terminals.registerAdapter(
        std::make_shared<paybridge::terminal::VerifoneVx680SimulatorAdapter>()));
    assert(terminals.registerAdapter(
        std::make_shared<paybridge::terminal::Vx680UnverifiedAdapter>()));
    paybridge::terminal::TerminalModel terminal;
    terminal.terminalId = "SIM-API-001";
    terminal.transportKind = paybridge::terminal::TransportKind::SIMULATED;
    terminals.registerTerminal(terminal);
    terminals.setTransport(terminal.terminalId,
        std::make_unique<paybridge::terminal::SimulatedTerminalTransport>(
            paybridge::terminal::SimulatedTerminalOutcome::APPROVED));

    const std::string token(64, 'a');
    const AuthManager auth = AuthManager::fromBearerToken(token);
    assert(auth.isConfigured());
    DashboardApiServer::Config config;
    config.port = 0;
    DashboardApiServer server(config, auth, engine, terminals, engine.assets());
    assert(server.start());

    const auto health = sendRequest(server.boundPort(), "/api/v1/health");
    assert(health.status == 200);
    const auto healthJson = parsed(health);
    assert(healthJson.size() == 2);
    assert(healthJson.value("mode", "") == "SIMULATION");
    assert(healthJson.value("persistence_ready", false));
    assert(health.body.find("database") == std::string::npos);
    assert(health.body.find("token") == std::string::npos);

    const std::vector<std::string> protectedTargets = {
        "/api/v1/transactions", "/api/v1/transactions/" + first.transactionId,
        "/api/v1/terminals", "/api/v1/assets", "/api/v1/adapters"
    };
    for (const auto& target : protectedTargets)
    {
        assert(sendRequest(server.boundPort(), target).status == 401);
        assert(sendRequest(server.boundPort(), target, "Basic abc").status == 401);
        assert(sendRequest(server.boundPort(), target, "Bearer malformed").status == 401);
        assert(sendRequest(server.boundPort(), target, "Bearer " + std::string(64, '0')).status == 401);
    }
    const std::string bearer = "Bearer " + token;

    const auto firstPage = sendRequest(server.boundPort(),
        "/api/v1/transactions?limit=1", bearer);
    assert(firstPage.status == 200);
    const auto firstPageJson = parsed(firstPage);
    assert(firstPageJson.at("transactions").size() == 1);
    assert(firstPageJson.at("page").at("limit") == 1);
    assert(firstPageJson.at("page").at("has_more").get<bool>());
    const std::string nextCursor = firstPageJson.at("page").at("next_cursor").get<std::string>();
    assert(!nextCursor.empty());
    assert(firstPage.body.find("PRIVATE-EXTERNAL-REFERENCE") == std::string::npos);
    assert(firstPage.body.find(token) == std::string::npos);
    assert(firstPage.body.find("canonical_request_hash") == std::string::npos);
    assert(firstPageJson.at("transactions").at(0).at("amount").at("units").is_string());

    const auto nextPage = sendRequest(server.boundPort(),
        "/api/v1/transactions?limit=1&cursor=" + nextCursor, bearer);
    assert(nextPage.status == 200);
    assert(parsed(nextPage).at("transactions").size() == 1);
    const auto statusFilter = sendRequest(server.boundPort(),
        "/api/v1/transactions?status=APPROVED", bearer);
    assert(statusFilter.status == 200);
    assert(parsed(statusFilter).at("transactions").size() == 1);

    for (const std::string& badQuery : {
        "?limit=0", "?limit=101", "?limit=nope", "?status=PROCESSING",
        "?cursor=-1:PB-123", "?cursor=1:bad/id", "?limit=1&limit=2", "?other=x"
    })
        assert(sendRequest(server.boundPort(), "/api/v1/transactions" + badQuery, bearer).status == 400);

    const auto detail = sendRequest(server.boundPort(),
        "/api/v1/transactions/" + first.transactionId, bearer);
    assert(detail.status == 200);
    const auto detailJson = parsed(detail);
    assert(detailJson.at("transaction_id") == first.transactionId);
    assert(detailJson.at("amount").at("units") == "1234567890123");
    assert(detailJson.at("asset").at("code") == "USD");
    assert(detailJson.at("status") == "CREATED");
    assert(detail.body.find("external_reference") == std::string::npos);
    assert(detail.body.find("PRIVATE-EXTERNAL-REFERENCE") == std::string::npos);
    assert(sendRequest(server.boundPort(), "/api/v1/transactions/not-found-id", bearer).status == 404);
    const auto missing = sendRequest(server.boundPort(), "/api/v1/transactions/not-found-id", bearer);
    assert(parsed(missing).at("error").at("message") == "Transaction not found");
    assert(missing.body.find("not-found-id") == std::string::npos);
    assert(sendRequest(server.boundPort(), "/api/v1/transactions/bad%2Fid", bearer).status == 400);

    const auto terminalResponse = sendRequest(server.boundPort(), "/api/v1/terminals", bearer);
    assert(terminalResponse.status == 200);
    const auto terminalJson = parsed(terminalResponse);
    assert(terminalJson.at("terminals").size() == 1);
    assert(terminalJson.at("terminals").at(0).at("terminal_id") == "SIM-API-001");
    assert(terminalJson.at("terminals").at(0).at("simulated").get<bool>());
    assert(terminalJson.at("terminals").at(0).at("available").get<bool>());

    const auto adapterResponse = sendRequest(server.boundPort(), "/api/v1/adapters", bearer);
    assert(adapterResponse.status == 200);
    const auto adapterJson = parsed(adapterResponse);
    assert(adapterJson.at("adapters").size() == 3);

    const auto findAdapter = [&](const std::string& id) -> const nlohmann::json&
    {
        for (const auto& item : adapterJson.at("adapters"))
        {
            if (item.at("adapter_id") == id)
                return item;
        }

        assert(false && "Expected adapter was not returned");
        return adapterJson.at("adapters").at(0);
    };

    const auto& simulatedAdapter = findAdapter("paybridge.simulated.v1");
    assert(simulatedAdapter.at("execution_scope") == "SIMULATION_ONLY");
    assert(simulatedAdapter.at("verification_status") == "SIMULATED");

    const auto& verifoneSimulator =
        findAdapter("paybridge.verifone.vx680.simulator.v1");
    assert(verifoneSimulator.at("execution_scope") == "SIMULATION_PROFILE_ONLY");
    assert(verifoneSimulator.at("verification_status") == "SIMULATED");

    const auto& unverifiedVerifone =
        findAdapter("paybridge.verifone.vx680.unverified");
    assert(unverifiedVerifone.at("execution_scope") == "BLOCKED_UNTIL_VERIFIED");
    assert(unverifiedVerifone.at("verification_status") == "UNVERIFIED");

    assert(sendRequest(server.boundPort(), "/api/v1/adapters",
        bearer, http::verb::post).status == 405);
    assert(sendRequest(server.boundPort(), "/api/v1/adapters?unexpected=value",
        bearer).status == 400);

    const auto assetResponse = sendRequest(server.boundPort(), "/api/v1/assets", bearer);
    assert(assetResponse.status == 200);
    const auto assetJson = parsed(assetResponse);
    assert(!assetJson.at("assets").empty());
    assert(assetResponse.body.find("PRIVATE-NETWORK") == std::string::npos);
    assert(assetResponse.body.find("PRIVATE-CONTRACT-DETAIL") == std::string::npos);
    assert(sendRequest(server.boundPort(), "/api/v1/assets", bearer, http::verb::post).status == 405);
    assert(sendRequest(server.boundPort(), "/api/v1/transactions", bearer, http::verb::delete_).status == 405);
    assert(sendRequest(server.boundPort(), "/api/v1/assets", bearer, http::verb::get, "{} ").status == 400);

    const auto tooLargeHeader = sendRawRequest(server.boundPort(),
        "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nX-Large: " +
        std::string(17000, 'x') + "\r\n\r\n");
    assert(tooLargeHeader.status == 431);
    const auto tooLargeBody = sendRawRequest(server.boundPort(),
        "POST /api/v1/assets HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5000\r\n\r\n");
    assert(tooLargeBody.status == 413);

    // Inject a runtime SQLite read failure into this disposable test database.
    // This does not modify the application's schema or any user database.
    sqlite3* failureConnection = nullptr;
    assert(sqlite3_open(databasePath.string().c_str(), &failureConnection) == SQLITE_OK);
    char* sqliteError = nullptr;
    const int dropResult = sqlite3_exec(failureConnection,
        "DROP TABLE transactions", nullptr, nullptr, &sqliteError);
    if (sqliteError) sqlite3_free(sqliteError);
    sqlite3_close(failureConnection);
    assert(dropResult == SQLITE_OK);

    const auto unhealthyHealth = sendRequest(server.boundPort(), "/api/v1/health");
    assert(unhealthyHealth.status == 200);
    assert(!parsed(unhealthyHealth).at("persistence_ready").get<bool>());
    const auto unavailableTransactions = sendRequest(server.boundPort(),
        "/api/v1/transactions", bearer);
    assert(unavailableTransactions.status == 503);
    assert(unavailableTransactions.body.find("SQLite") == std::string::npos);

    server.stop();
    server.join();
    assert(!server.isRunning());

    DashboardApiServer::Config limitedConfig;
    limitedConfig.port = 0;
    limitedConfig.workerCount = 0;       // Normalized to the minimum of one.
    limitedConfig.maximumConnections = 2;
    DashboardApiServer limitedServer(limitedConfig);
    assert(limitedServer.start());
    asio::io_context limitContext;
    tcp::socket heldConnection(limitContext);
    heldConnection.connect({asio::ip::make_address_v4("127.0.0.1"), limitedServer.boundPort()});
    const std::string partialRequest = "GET /api/v1/health HTTP/1.1\r\nHost:";
    asio::write(heldConnection, asio::buffer(partialRequest));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // One worker continues to serve a complete request while another session
    // is waiting for the rest of its bounded request.
    assert(sendRequest(limitedServer.boundPort(), "/api/v1/health").status == 200);

    tcp::socket secondHeldConnection(limitContext);
    secondHeldConnection.connect({asio::ip::make_address_v4("127.0.0.1"), limitedServer.boundPort()});
    asio::write(secondHeldConnection, asio::buffer(partialRequest));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    tcp::socket overflowConnection(limitContext);
    overflowConnection.connect({asio::ip::make_address_v4("127.0.0.1"), limitedServer.boundPort()});
    overflowConnection.non_blocking(true);
    bool overflowRejected = false;
    char overflowByte = 0;
    for (int attempt = 0; attempt < 50 && !overflowRejected; ++attempt)
    {
        boost::system::error_code error;
        const std::size_t received = overflowConnection.read_some(
            asio::buffer(&overflowByte, 1), error);
        if (received > 0)
            break;
        if (error && error != asio::error::would_block && error != asio::error::try_again)
            overflowRejected = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(overflowRejected);
    overflowConnection.close();
    secondHeldConnection.close();
    heldConnection.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(sendRequest(limitedServer.boundPort(), "/api/v1/health").status == 200);
    limitedServer.stop();
    limitedServer.join();
    assert(!limitedServer.isRunning());

    // ---- POST /api/v1/simulator/payments ----
    {
        namespace term = paybridge::terminal;
        const auto fail = [](const std::string& what)
        {
            std::cerr << "POST-ROUTE TEST FAILED: " << what << std::endl;
            std::exit(1);
        };
        const auto expect = [&fail](bool condition, const std::string& what)
        {
            if (!condition) fail(what);
        };

        const auto payRoot = root / "payments";
        std::filesystem::create_directories(payRoot);
        const std::string payDb = (payRoot / "payments.sqlite").string();
        std::vector<std::pair<std::string, std::string>> recorded;  // transaction id, expected status
        const std::string payToken(64, 'b');
        const std::string payBearer = "Bearer " + payToken;

        {
            TransactionEngine payEngine(payDb, {});
            expect(payEngine.initialize(), "payment test engine failed to initialize");

            term::TerminalManager payTerminals;
            expect(payTerminals.registerAdapter(std::make_shared<term::SimulatedTerminalAdapter>()),
                "could not register simulated adapter");
            auto approveSends = std::make_shared<std::atomic<int>>(0);
            const auto addSimulated = [&](const std::string& id,
                std::unique_ptr<paybridge::transport::ITransport> transport)
            {
                term::TerminalModel model;
                model.terminalId = id;
                model.transportKind = term::TransportKind::SIMULATED;
                expect(payTerminals.registerTerminal(model), "could not register " + id);
                payTerminals.setTransport(id, std::move(transport));
            };
            addSimulated("SIM-APPROVE", std::make_unique<PostRouteCountingTransport>(
                term::SimulatedTerminalOutcome::APPROVED, approveSends));
            addSimulated("SIM-DECLINE", std::make_unique<term::SimulatedTerminalTransport>(
                term::SimulatedTerminalOutcome::DECLINED));
            addSimulated("SIM-TIMEOUT", std::make_unique<term::SimulatedTerminalTransport>(
                term::SimulatedTerminalOutcome::TIMEOUT));
            addSimulated("SIM-DISCONNECT", std::make_unique<term::SimulatedTerminalTransport>(
                term::SimulatedTerminalOutcome::DISCONNECTED));
            term::TerminalModel tcpModel;
            tcpModel.terminalId = "TCP-NOT-SIM";
            tcpModel.transportKind = term::TransportKind::TCP;
            expect(payTerminals.registerTerminal(tcpModel), "could not register TCP terminal");

            PaymentOrchestrationService payService(payEngine, payTerminals);
            const AuthManager payAuth = AuthManager::fromBearerToken(payToken);
            DashboardApiServer::Config payConfig;
            payConfig.port = 0;
            DashboardApiServer payServer(payConfig, payAuth, payEngine, payTerminals,
                payEngine.assets(), payService);
            expect(payServer.start(), "payment test server failed to start");
            const std::uint16_t port = payServer.boundPort();

            const auto post = [port](const std::optional<std::string>& authorization,
                const std::string& contentType, const std::string& body)
            {
                asio::io_context context;
                tcp::socket socket(context);
                socket.connect({asio::ip::make_address_v4("127.0.0.1"), port});
                http::request<http::string_body> request{
                    http::verb::post, "/api/v1/simulator/payments", 11};
                request.set(http::field::host, "127.0.0.1");
                if (authorization) request.set(http::field::authorization, *authorization);
                if (!contentType.empty()) request.set(http::field::content_type, contentType);
                request.body() = body;
                request.prepare_payload();
                http::write(socket, request);
                beast::flat_buffer buffer;
                http::response<http::string_body> response;
                http::read(socket, buffer, response);
                return HttpResult{response.result_int(), response.body()};
            };
            const auto nowSeconds = []() -> std::int64_t
            {
                return std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            };
            const auto makeBody = [](const std::string& requestId, const std::string& reference,
                const std::string& amount, const std::string& terminal, std::int64_t timestamp)
            {
                nlohmann::json body = {
                    {"protocol_version", "1.0"}, {"message_type", "PAYMENT_REQUEST"},
                    {"external_reference", reference}, {"request_id", requestId},
                    {"request_timestamp", timestamp}, {"request_expiry", timestamp + 300},
                    {"payment", {{"amount", amount},
                        {"asset", {{"type", "FIAT"}, {"code", "USD"}}}}}
                };
                if (!terminal.empty()) body["terminal_id"] = terminal;
                return body.dump();
            };
            const auto transactionCount = [&]() -> std::size_t
            {
                const auto listed = sendRequest(port, "/api/v1/transactions?limit=100", payBearer);
                expect(listed.status == 200, "transaction list failed: " + listed.body);
                return nlohmann::json::parse(listed.body).at("transactions").size();
            };
            const auto errorCodeOf = [](const HttpResult& result) -> std::string
            {
                try { return nlohmann::json::parse(result.body).at("error").at("code").get<std::string>(); }
                catch (...) { return "<unparseable>"; }
            };
            int referenceNumber = 0;
            const auto nextReference = [&referenceNumber]()
            {
                return "POST-ROUTE-PRIVATE-REF-" + std::to_string(++referenceNumber);
            };

            const auto runScenario = [&](const std::string& terminal,
                const std::string& expectedStatus, const std::string& expectedResultCode)
            {
                const std::string reference = nextReference();
                const auto response = post(payBearer, "application/json",
                    makeBody(JsonHandler::generateRequestId(), reference, "12.34", terminal, nowSeconds()));
                const std::string label = terminal + " expected " + expectedStatus +
                    " but got HTTP " + std::to_string(response.status) + " " + response.body;
                expect(response.status == 200, label);
                const auto json = nlohmann::json::parse(response.body);
                expect(json.value("status", "") == expectedStatus, label);
                expect(json.value("result_code", "") == expectedResultCode, label);
                expect(json.value("simulated", false), terminal + " response must be marked simulated");
                expect(response.body.find(reference) == std::string::npos,
                    "external_reference leaked in response for " + terminal);
                expect(response.body.find(payToken) == std::string::npos, "token leaked in response");
                const std::string id = json.value("transaction_id", "");
                expect(!id.empty(), "no transaction_id for " + terminal);
                const auto stored = sendRequest(port, "/api/v1/transactions/" + id, payBearer);
                expect(stored.status == 200, "GET detail failed for " + terminal + ": " + stored.body);
                expect(nlohmann::json::parse(stored.body).value("status", "") == expectedStatus,
                    "persisted status mismatch for " + terminal + ": " + stored.body);
                recorded.emplace_back(id, expectedStatus);
            };

            // Terminal outcomes.
            runScenario("SIM-APPROVE", "APPROVED", "SIMULATED_APPROVED");
            runScenario("SIM-DECLINE", "DECLINED", "SIMULATED_DECLINED");
            runScenario("SIM-TIMEOUT", "UNKNOWN", "SIMULATED_RESPONSE_HEADER_INCOMPLETE");
            runScenario("SIM-DISCONNECT", "FAILED", "TERMINAL_CONNECT_FAILED");

            // Terminal selection is simulator-only and creates no transaction.
            const std::size_t beforeSelection = transactionCount();
            for (const std::string& terminal : {std::string("SIM-NOPE"), std::string("TCP-NOT-SIM"), std::string()})
            {
                const auto response = post(payBearer, "application/json",
                    makeBody(JsonHandler::generateRequestId(), nextReference(), "1.00", terminal, nowSeconds()));
                expect(response.status == 422 && errorCodeOf(response) == "TERMINAL_UNAVAILABLE",
                    "terminal '" + terminal + "' should return 422 TERMINAL_UNAVAILABLE, got " +
                    std::to_string(response.status) + " " + response.body);
            }
            expect(transactionCount() == beforeSelection, "terminal-selection failures created transactions");

            // Authentication happens before anything else and creates nothing.
            const std::size_t beforeAuth = transactionCount();
            const std::string validBody = makeBody(JsonHandler::generateRequestId(), nextReference(),
                "5.00", "SIM-APPROVE", nowSeconds());
            expect(post(std::nullopt, "application/json", validBody).status == 401, "missing auth must be 401");
            expect(post(std::string("Basic abc"), "application/json", validBody).status == 401, "Basic auth must be 401");
            expect(post(std::string("Bearer malformed"), "application/json", validBody).status == 401, "malformed Bearer must be 401");
            expect(post("Bearer " + std::string(64, 'c'), "application/json", validBody).status == 401, "wrong token must be 401");
            expect(sendRequest(port, "/api/v1/simulator/payments").status == 401, "unauthenticated GET must be 401");
            expect(transactionCount() == beforeAuth, "rejected requests created transactions");

            // Request validation.
            expect(sendRequest(port, "/api/v1/simulator/payments", payBearer).status == 405, "GET with auth must be 405");
            expect(post(payBearer, "text/plain", validBody).status == 415, "text/plain must be 415");
            expect(post(payBearer, "", validBody).status == 415, "missing content type must be 415");
            const auto badJson = post(payBearer, "application/json", "{not json");
            expect(badJson.status == 400 && errorCodeOf(badJson) == "INVALID_JSON",
                "malformed JSON should be 400 INVALID_JSON, got " + badJson.body);
            const auto stale = post(payBearer, "application/json", makeBody(JsonHandler::generateRequestId(),
                nextReference(), "5.00", "SIM-APPROVE", nowSeconds() - 1000));
            expect(stale.status == 400 && errorCodeOf(stale) == "REQUEST_TIMESTAMP_INVALID",
                "stale timestamp should be 400 REQUEST_TIMESTAMP_INVALID, got " + stale.body);
            const auto withQuery = sendRequest(port, "/api/v1/simulator/payments?x=1", payBearer,
                http::verb::post, validBody);
            expect(withQuery.status == 400 && errorCodeOf(withQuery) == "INVALID_QUERY",
                "query string should be 400 INVALID_QUERY, got " + withQuery.body);
            expect(transactionCount() == beforeAuth, "invalid requests created transactions");

            // Replay: identical retry returns the stored result without a second send.
            const std::string replayId = JsonHandler::generateRequestId();
            const std::string replayReference = nextReference();
            const std::string replayBody = makeBody(replayId, replayReference, "7.00", "SIM-APPROVE", nowSeconds());
            const int sendsBefore = approveSends->load();
            const auto firstReplay = post(payBearer, "application/json", replayBody);
            const auto secondReplay = post(payBearer, "application/json", replayBody);
            expect(firstReplay.status == 200 && secondReplay.status == 200,
                "replay calls should both be 200: " + firstReplay.body + " / " + secondReplay.body);
            const auto firstJson = nlohmann::json::parse(firstReplay.body);
            const auto secondJson = nlohmann::json::parse(secondReplay.body);
            expect(firstJson.value("status", "") == "APPROVED" && secondJson.value("status", "") == "APPROVED",
                "replay statuses should be APPROVED: " + secondReplay.body);
            expect(firstJson.value("transaction_id", "a") == secondJson.value("transaction_id", "b"),
                "identical retry must return the same transaction");
            expect(approveSends->load() - sendsBefore == 1, "identical retry must not reach the terminal again");
            recorded.emplace_back(firstJson.value("transaction_id", ""), "APPROVED");

            // Replay: same request_id with a changed payment is a conflict.
            const auto conflict = post(payBearer, "application/json",
                makeBody(replayId, replayReference, "99.99", "SIM-APPROVE", nowSeconds()));
            const std::string conflictCode = errorCodeOf(conflict);
            expect(conflict.status == 409 &&
                    (conflictCode == "REPLAY_CONFLICT" || conflictCode == "EXTERNAL_REFERENCE_CONFLICT"),
                "changed payment under same request_id should be 409, got " +
                std::to_string(conflict.status) + " " + conflict.body);
            expect(approveSends->load() - sendsBefore == 1, "conflicting replay must not reach the terminal");

            payServer.stop();
            payServer.join();
            expect(!payServer.isRunning(), "payment test server did not stop");
        }

        // Durable persistence: reopen the same database file in a new engine.
        TransactionEngine reopened(payDb, {});
        expect(reopened.initialize(), "reopened engine failed to initialize");
        for (const auto& [id, status] : recorded)
        {
            const auto record = reopened.findTransactionById(id);
            expect(record.has_value(), "transaction missing after reopen: " + id);
            expect(TransactionEngine::statusToString(record->status) == status,
                "status changed after reopen for " + id + ": expected " + status);
        }
        std::cout << "Simulator payment route tests passed ("
                  << recorded.size() << " transactions verified after reopen)\n";
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    WSACleanup();
    std::cout << "Dashboard authenticated routes, redaction, limits, and shutdown passed\n";
    return 0;
}
