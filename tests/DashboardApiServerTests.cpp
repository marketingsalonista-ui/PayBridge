#include "../src/communication/DashboardApiServer.h"
#include "../src/core/TransactionEngine.h"
#include "../src/security/AuthManager.h"
#include "../src/terminal/SimulatedTerminalTransport.h"
#include "../src/terminal/TerminalManager.h"

#include <winsock2.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cassert>
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
        "/api/v1/terminals", "/api/v1/assets"
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

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    WSACleanup();
    std::cout << "Dashboard authenticated routes, redaction, limits, and shutdown passed\n";
    return 0;
}
