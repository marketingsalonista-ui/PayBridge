#include "DashboardApiServer.h"

#include "../assets/AssetRegistry.h"
#include "../core/TransactionEngine.h"
#include "../security/AuthManager.h"
#include "../terminal/TerminalManager.h"
#include "../core/PaymentOrchestrationService.h"
#include "../protocol/JsonHandler.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace
{
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using ErrorCode = boost::system::error_code;

constexpr std::size_t MAX_HEADER_BYTES = 16 * 1024;
constexpr std::size_t MAX_BODY_BYTES = 4 * 1024;
constexpr std::size_t MAX_PAGE_SIZE = 100;
constexpr auto IO_TIMEOUT = std::chrono::seconds(5);
using HttpResponse = http::response<http::string_body>;
using HttpRequest = http::request<http::string_body>;

HttpResponse errorResponse(
    http::status status, unsigned version, const std::string& code,
    const std::string& message)
{
    http::response<http::string_body> response{status, version};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    if (status == http::status::unauthorized)
        response.set(http::field::www_authenticate, "Bearer");
    response.keep_alive(false);
    response.body() = nlohmann::json{
        {"error", {{"code", code}, {"message", message}}}
    }.dump();
    response.prepare_payload();
    return response;
}

HttpResponse jsonResponse(http::status status, unsigned version, const nlohmann::json& body)
{
    HttpResponse response{status, version};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.keep_alive(false);
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

bool validTransactionId(const std::string& value)
{
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character)
    {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') ||
               character == '-' || character == '_';
    });
}

bool parseUnsigned(const std::string& value, std::uint64_t& output)
{
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

struct ParsedQuery
{
    bool valid = true;
    std::size_t limit = 50;
    std::optional<TransactionStatus> status;
    std::optional<TransactionCursor> cursor;
};

ParsedQuery parseTransactionQuery(const std::string& query)
{
    ParsedQuery parsed;
    if (query.empty()) return parsed;
    if (query.find('%') != std::string::npos || query.find('#') != std::string::npos)
    { parsed.valid = false; return parsed; }

    std::unordered_map<std::string, std::string> values;
    std::size_t begin = 0;
    while (begin <= query.size())
    {
        const std::size_t end = query.find('&', begin);
        const std::string pair = query.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        const std::size_t equals = pair.find('=');
        if (equals == std::string::npos || equals == 0 || equals + 1 == pair.size())
        { parsed.valid = false; return parsed; }
        const std::string key = pair.substr(0, equals);
        const std::string value = pair.substr(equals + 1);
        if ((key != "limit" && key != "status" && key != "cursor") ||
            !values.emplace(key, value).second)
        { parsed.valid = false; return parsed; }
        if (end == std::string::npos) break;
        begin = end + 1;
    }

    if (const auto found = values.find("limit"); found != values.end())
    {
        std::uint64_t value = 0;
        if (!parseUnsigned(found->second, value) || value == 0 || value > MAX_PAGE_SIZE)
        { parsed.valid = false; return parsed; }
        parsed.limit = static_cast<std::size_t>(value);
    }
    if (const auto found = values.find("status"); found != values.end())
    {
        const std::string& value = found->second;
        if (value == "CREATED") parsed.status = TransactionStatus::CREATED;
        else if (value == "SENT") parsed.status = TransactionStatus::SENT;
        else if (value == "RECEIVED") parsed.status = TransactionStatus::RECEIVED;
        else if (value == "APPROVED") parsed.status = TransactionStatus::APPROVED;
        else if (value == "DECLINED") parsed.status = TransactionStatus::DECLINED;
        else if (value == "FAILED") parsed.status = TransactionStatus::FAILED;
        else if (value == "UNKNOWN") parsed.status = TransactionStatus::UNKNOWN;
        else if (value == "INVALID") parsed.status = TransactionStatus::INVALID;
        else { parsed.valid = false; return parsed; }
    }
    if (const auto found = values.find("cursor"); found != values.end())
    {
        const std::size_t separator = found->second.find(':');
        if (separator == std::string::npos ||
            found->second.find(':', separator + 1) != std::string::npos)
        { parsed.valid = false; return parsed; }
        std::uint64_t timestamp = 0;
        const std::string time = found->second.substr(0, separator);
        const std::string id = found->second.substr(separator + 1);
        if (!parseUnsigned(time, timestamp) ||
            timestamp > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            !validTransactionId(id))
        { parsed.valid = false; return parsed; }
        parsed.cursor = TransactionCursor{static_cast<std::int64_t>(timestamp), id};
    }
    return parsed;
}

nlohmann::json transactionDto(const TransactionReadRecord& record)
{
    nlohmann::json dto = {
        {"transaction_id", record.transactionId},
        {"status", TransactionEngine::statusToString(record.status)},
        {"created_at_unix", record.createdAtUnix},
        {"updated_at_unix", record.updatedAtUnix},
        {"amount", {{"units", std::to_string(record.money.units)},
                    {"precision", record.money.precision}}},
        {"asset", {{"code", record.asset.code},
                   {"type", paybridge::assets::assetTypeToString(record.asset.type)},
                   {"precision", record.asset.decimalPrecision}}}
    };
    if (!record.terminalId.empty()) dto["terminal_id"] = record.terminalId;
    return dto;
}
}

class DashboardApiServer::Impl
{
    class Session;

    static Config normalize(Config value)
    {
        value.workerCount = std::clamp<std::size_t>(value.workerCount, 1, 8);
        value.maximumConnections = std::clamp<std::size_t>(value.maximumConnections, 1, 128);
        return value;
    }

public:
    explicit Impl(Config value, AuthManager auth = AuthManager::fromEnvironment(),
        TransactionEngine* engine = nullptr,
        const paybridge::terminal::TerminalManager* terminals = nullptr,
        const paybridge::assets::AssetRegistry* assets = nullptr,
        PaymentOrchestrationService* payments = nullptr)
        : config(normalize(std::move(value))), context(config.workerCount),
          acceptor(context), authManager(std::move(auth)),
          transactionEngine(engine), terminalManager(terminals), assetRegistry(assets),
          paymentService(payments)
    {
    }

    ~Impl()
    {
        requestStop();
        join();
    }

    bool isRunning() const noexcept { return running.load(); }
    std::uint16_t boundPort() const noexcept { return boundPortValue; }

    bool start()
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
        if (started || joined) return false;

#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            lastError = "Winsock initialization failed";
            return false;
        }
        winsockStarted = true;
#endif

        ErrorCode error;
        acceptor.open(tcp::v4(), error);
        if (!error) acceptor.set_option(asio::socket_base::reuse_address(false), error);
        if (!error)
            acceptor.bind(tcp::endpoint(asio::ip::address_v4::loopback(), config.port), error);
        if (!error) acceptor.listen(asio::socket_base::max_listen_connections, error);
        if (error)
        {
            lastError = error.message();
            ErrorCode ignored;
            acceptor.close(ignored);
#ifdef _WIN32
            WSACleanup();
            winsockStarted = false;
#endif
            return false;
        }

        boundPortValue = acceptor.local_endpoint(error).port();
        if (error)
        {
            lastError = error.message();
            ErrorCode ignored;
            acceptor.close(ignored);
#ifdef _WIN32
            WSACleanup();
            winsockStarted = false;
#endif
            return false;
        }

        workGuard.emplace(asio::make_work_guard(context));
        started = true;
        scheduleAccept();
        try
        {
            workers.reserve(config.workerCount);
            for (std::size_t i = 0; i < config.workerCount; ++i)
                workers.emplace_back([this] { context.run(); });
        }
        catch (...)
        {
            lastError = "Could not start HTTP worker threads";
            requestStop();
            return false;
        }

        running = true;
        return true;
    }

    void requestStop() noexcept
    {
        if (!started || stopping.exchange(true)) return;
        asio::post(context, [this]
        {
            ErrorCode ignored;
            acceptor.cancel(ignored);
            acceptor.close(ignored);

            std::vector<std::shared_ptr<Session>> active;
            {
                std::lock_guard<std::mutex> lock(sessionsMutex);
                active.reserve(sessions.size());
                for (const auto& entry : sessions)
                    active.push_back(entry.second);
            }
            for (const auto& session : active) session->stop();
            if (workGuard) workGuard->reset();
        });
    }

    void join() noexcept
    {
        if (started && !stopping.load()) requestStop();
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
        if (joined) return;
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
        workers.clear();
        running = false;
        joined = true;
#ifdef _WIN32
        if (winsockStarted)
        {
            WSACleanup();
            winsockStarted = false;
        }
#endif
    }

private:
    HttpResponse handleRequest(const HttpRequest& request) const
    {
        const std::string target(request.target());
        if (target.empty() || target.front() != '/' || target.find('#') != std::string::npos)
            return errorResponse(http::status::bad_request, request.version(),
                "INVALID_PATH", "Request path is invalid");
        const std::size_t question = target.find('?');
        const std::string path = target.substr(0, question);
        const std::string query = question == std::string::npos
            ? std::string{} : target.substr(question + 1);

        if (path == "/api/v1/health")
        {
            if (request.method() != http::verb::get)
                return errorResponse(http::status::method_not_allowed, request.version(),
                    "METHOD_NOT_ALLOWED", "Only GET is supported");
            if (!request.body().empty())
                return errorResponse(http::status::bad_request, request.version(),
                    "BODY_NOT_ALLOWED", "Health endpoint does not accept a request body");
            if (!query.empty())
                return errorResponse(http::status::bad_request, request.version(),
                    "INVALID_QUERY", "Health endpoint does not accept query parameters");
            return jsonResponse(http::status::ok, request.version(), nlohmann::json{
                {"mode", "SIMULATION"},
                {"persistence_ready", transactionEngine && transactionEngine->isReady()}
            });
        }

        constexpr const char* TRANSACTIONS = "/api/v1/transactions";
        constexpr const char* DETAIL_PREFIX = "/api/v1/transactions/";
        const bool listRoute = path == TRANSACTIONS;
        const bool detailRoute = path.rfind(DETAIL_PREFIX, 0) == 0;
        const bool terminalsRoute = path == "/api/v1/terminals";
        const bool assetsRoute = path == "/api/v1/assets";
        const bool paymentsRoute = path == "/api/v1/simulator/payments";
        if (!listRoute && !detailRoute && !terminalsRoute && !assetsRoute && !paymentsRoute)
            return errorResponse(http::status::not_found, request.version(),
                "NOT_FOUND", "Endpoint not found");

        if (request.count(http::field::authorization) > 1)
            return errorResponse(http::status::unauthorized, request.version(),
                "UNAUTHORIZED", "Bearer authentication is required");
        std::string authorization;
        const auto authValue = request[http::field::authorization];
        if (!authValue.empty()) authorization = "Authorization: " + std::string(authValue);
        if (authorization.empty())
            return errorResponse(http::status::unauthorized, request.version(),
                "UNAUTHORIZED", "Bearer authentication is required");
        const auto verification = authManager.verifyAuthorization(authorization);
        if (verification == AuthManager::VerificationResult::MISSING ||
            verification == AuthManager::VerificationResult::MALFORMED ||
            verification == AuthManager::VerificationResult::INVALID)
            return errorResponse(http::status::unauthorized, request.version(),
                "UNAUTHORIZED", "Bearer authentication is required");
        if (verification != AuthManager::VerificationResult::ALLOWED)
            return errorResponse(http::status::service_unavailable, request.version(),
                "AUTH_UNAVAILABLE", "Authentication service is unavailable");
        if (paymentsRoute)
            return handleSimulatorPayment(request, query);

        if (request.method() != http::verb::get)
            return errorResponse(http::status::method_not_allowed, request.version(),
                "METHOD_NOT_ALLOWED", "Only GET is supported");
        if (!request.body().empty())
            return errorResponse(http::status::bad_request, request.version(),
                "BODY_NOT_ALLOWED", "GET endpoints do not accept a request body");

        if (target.find('%') != std::string::npos)
            return errorResponse(http::status::bad_request, request.version(),
                "INVALID_PATH", "Encoded paths and query values are not supported");

        if (listRoute)
        {
            const ParsedQuery parsed = parseTransactionQuery(query);
            if (!parsed.valid)
                return errorResponse(http::status::bad_request, request.version(),
                    "INVALID_QUERY", "Transaction query parameters are invalid");
            if (!transactionEngine || !transactionEngine->isReady())
                return errorResponse(http::status::service_unavailable, request.version(),
                    "PERSISTENCE_UNAVAILABLE", "Transaction data is unavailable");
            const TransactionPage page = transactionEngine->listTransactions(
                parsed.limit, parsed.status, parsed.cursor);
            if (!page.success || !page.validRequest)
                return errorResponse(http::status::service_unavailable, request.version(),
                    "PERSISTENCE_UNAVAILABLE", "Transaction data is unavailable");
            nlohmann::json transactions = nlohmann::json::array();
            for (const auto& record : page.transactions)
                transactions.push_back(transactionDto(record));
            nlohmann::json response = {
                {"transactions", std::move(transactions)},
                {"page", {{"limit", parsed.limit}, {"has_more", page.hasMore},
                           {"next_cursor", nullptr}}}
            };
            if (page.nextCursor)
                response["page"]["next_cursor"] =
                    std::to_string(page.nextCursor->createdAtUnix) + ":" +
                    page.nextCursor->transactionId;
            return jsonResponse(http::status::ok, request.version(), response);
        }

        if (detailRoute)
        {
            if (!query.empty())
                return errorResponse(http::status::bad_request, request.version(),
                    "INVALID_QUERY", "Transaction detail does not accept query parameters");
            const std::string id = path.substr(std::char_traits<char>::length(DETAIL_PREFIX));
            if (!validTransactionId(id))
                return errorResponse(http::status::bad_request, request.version(),
                    "INVALID_TRANSACTION_ID", "Transaction ID is invalid");
            if (!transactionEngine || !transactionEngine->isReady())
                return errorResponse(http::status::service_unavailable, request.version(),
                    "PERSISTENCE_UNAVAILABLE", "Transaction data is unavailable");
            const auto record = transactionEngine->findTransactionById(id);
            if (!record)
                return errorResponse(http::status::not_found, request.version(),
                    "NOT_FOUND", "Transaction not found");
            return jsonResponse(http::status::ok, request.version(), transactionDto(*record));
        }

        if (!query.empty())
            return errorResponse(http::status::bad_request, request.version(),
                "INVALID_QUERY", "Endpoint does not accept query parameters");
        if (terminalsRoute)
        {
            nlohmann::json values = nlohmann::json::array();
            if (terminalManager)
                for (const auto& terminal : terminalManager->terminalSnapshots())
                    values.push_back({
                        {"terminal_id", terminal.terminalId},
                        {"simulated", terminal.simulated},
                        {"configured", terminal.configured},
                        {"connection_status", terminal.connectionStatus},
                        {"available", terminal.available}
                    });
            return jsonResponse(http::status::ok, request.version(), {{"terminals", values}});
        }

        nlohmann::json values = nlohmann::json::array();
        if (assetRegistry)
            for (const auto& asset : assetRegistry->readModels())
                values.push_back({
                    {"code", asset.code}, {"type", asset.type},
                    {"precision", asset.precision}, {"enabled", asset.enabled}
                });
        return jsonResponse(http::status::ok, request.version(), {{"assets", values}});
    }

    // POST /api/v1/simulator/payments. Reached only after Bearer verification.
    // Simulator-only: the terminal must resolve through selectSimulatedTerminal
    // before the orchestration service is called, so no other adapter is
    // reachable from here. Adapter preflight is untouched.
    // Note: this runs synchronously on an HTTP worker thread, which is
    // acceptable for simulated terminals only.
    HttpResponse handleSimulatorPayment(const HttpRequest& request, const std::string& query) const
    {
        const unsigned version = request.version();
        if (request.method() != http::verb::post)
            return errorResponse(http::status::method_not_allowed, version,
                "METHOD_NOT_ALLOWED", "Only POST is supported");
        if (!query.empty())
            return errorResponse(http::status::bad_request, version,
                "INVALID_QUERY", "Endpoint does not accept query parameters");
        if (!paymentService || !transactionEngine || !terminalManager ||
            !transactionEngine->isReady())
            return errorResponse(http::status::service_unavailable, version,
                "PAYMENTS_UNAVAILABLE", "Simulated payments are unavailable");

        const std::string contentType(request[http::field::content_type]);
        if (contentType != "application/json" && contentType.rfind("application/json;", 0) != 0)
            return errorResponse(http::status::unsupported_media_type, version,
                "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        if (request.body().empty())
            return errorResponse(http::status::bad_request, version,
                "INVALID_BODY", "A JSON request body is required");

        const PaymentValidation validation = JsonHandler::parseAndValidatePaymentRequest(
            request.body(), transactionEngine->assets());
        if (!validation.valid)
            return errorResponse(http::status::bad_request, version,
                validation.errorCode, validation.message);

        std::string terminalId, selectionError;
        if (!terminalManager->selectSimulatedTerminal(
                validation.request.terminalId, terminalId, selectionError))
            return errorResponse(http::status::unprocessable_entity, version,
                "TERMINAL_UNAVAILABLE", "No matching simulated terminal is available");

        const PaymentOrchestrationResult result =
            paymentService->process(validation.request, terminalId);

        if (result.status == "REJECTED")
        {
            const bool conflict = result.errorCode == "REPLAY_CONFLICT" ||
                result.errorCode == "EXTERNAL_REFERENCE_CONFLICT";
            return errorResponse(
                conflict ? http::status::conflict : http::status::service_unavailable, version,
                result.errorCode.empty() ? "PERSISTENCE_FAILURE" : result.errorCode,
                result.message);
        }

        nlohmann::json body = {
            {"request_id", validation.request.envelope.requestId},
            {"status", result.status},
            {"message", result.message},
            {"simulated", result.simulated}
        };
        if (!result.transactionId.empty()) body["transaction_id"] = result.transactionId;
        if (!result.terminalId.empty()) body["terminal_id"] = result.terminalId;
        if (!result.resultCode.empty()) body["result_code"] = result.resultCode;
        return jsonResponse(result.status == "PROCESSING" ? http::status::accepted
                                                          : http::status::ok,
            version, body);
    }

    class Session : public std::enable_shared_from_this<Session>
    {
    public:
        Session(tcp::socket socket, std::function<void()> completed,
            std::function<HttpResponse(const HttpRequest&)> handler)
            : stream(std::move(socket)), completed(std::move(completed)),
              handler(std::move(handler))
        {
            parser.header_limit(MAX_HEADER_BYTES);
            parser.body_limit(MAX_BODY_BYTES);
        }

        void start()
        {
            stream.expires_after(IO_TIMEOUT);
            http::async_read(stream, buffer, parser,
                [self = shared_from_this()](ErrorCode error, std::size_t)
                {
                    if (error)
                    {
                        const auto status = error == http::error::header_limit
                            ? http::status::request_header_fields_too_large
                            : error == http::error::body_limit
                            ? http::status::payload_too_large
                            : http::status::bad_request;
                        const std::string code = error == http::error::header_limit
                            ? "HEADERS_TOO_LARGE"
                            : error == http::error::body_limit
                            ? "BODY_TOO_LARGE" : "BAD_REQUEST";
                        self->write(errorResponse(status, 11, code,
                            error == http::error::header_limit ? "Request headers exceed the size limit"
                            : error == http::error::body_limit ? "Request body exceeds the size limit"
                            : "Invalid HTTP request"));
                        return;
                    }
                    try
                    {
                        self->write(self->handler(self->parser.get()));
                    }
                    catch (...)
                    {
                        self->write(errorResponse(http::status::internal_server_error, 11,
                            "INTERNAL_ERROR", "Request could not be completed"));
                    }
                });
        }

        void stop() noexcept
        {
            asio::dispatch(stream.get_executor(), [self = shared_from_this()]
            {
                self->closeNow();
            });
        }

        void closeNow() noexcept
        {
            ErrorCode ignored;
            stream.socket().cancel(ignored);
            stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
            stream.socket().close(ignored);
            complete();
        }

    private:
        void write(http::response<http::string_body> response)
        {
            outgoing = std::move(response);
            stream.expires_after(IO_TIMEOUT);
            http::async_write(stream, outgoing,
                [self = shared_from_this()](ErrorCode, std::size_t)
                {
                    self->closeNow();
                });
        }

        void complete() noexcept
        {
            if (completedOnce.exchange(true)) return;
            try { completed(); } catch (...) {}
        }

        beast::tcp_stream stream;
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        http::response<http::string_body> outgoing;
        std::function<void()> completed;
        std::function<HttpResponse(const HttpRequest&)> handler;
        std::atomic_bool completedOnce{false};
    };

    void scheduleAccept()
    {
        if (stopping.load()) return;
        acceptor.async_accept(asio::make_strand(context),
            [this](ErrorCode error, tcp::socket socket)
            {
                if (!error && !stopping.load())
                {
                    const std::size_t previous = activeConnections.fetch_add(1);
                    if (previous >= config.maximumConnections)
                    {
                        activeConnections.fetch_sub(1);
                        ErrorCode ignored;
                        socket.close(ignored);
                    }
                    else
                    {
                        const std::uint64_t id = nextSessionId.fetch_add(1);
                        auto session = std::make_shared<Session>(std::move(socket), [this, id]
                        {
                            {
                                std::lock_guard<std::mutex> lock(sessionsMutex);
                                sessions.erase(id);
                            }
                            activeConnections.fetch_sub(1);
                        }, [this](const HttpRequest& request) { return handleRequest(request); });
                        {
                            std::lock_guard<std::mutex> lock(sessionsMutex);
                            sessions.emplace(id, session);
                        }
                        session->start();
                    }
                }
                if (!stopping.load()) scheduleAccept();
            });
    }

    Config config;
    asio::io_context context;
    tcp::acceptor acceptor;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> workGuard;
    std::vector<std::thread> workers;
    mutable std::mutex sessionsMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;
    std::atomic_size_t activeConnections{0};
    std::atomic_uint64_t nextSessionId{1};
    std::atomic_bool stopping{false};
    std::atomic_bool running{false};
    std::mutex lifecycleMutex;
    bool started = false;
    bool joined = false;
    std::uint16_t boundPortValue = 0;
    std::string lastError;
    AuthManager authManager;
    TransactionEngine* transactionEngine = nullptr;
    const paybridge::terminal::TerminalManager* terminalManager = nullptr;
    const paybridge::assets::AssetRegistry* assetRegistry = nullptr;
    PaymentOrchestrationService* paymentService = nullptr;
#ifdef _WIN32
    bool winsockStarted = false;
#endif
};

DashboardApiServer::DashboardApiServer()
    : DashboardApiServer(Config{})
{
}

DashboardApiServer::DashboardApiServer(Config config)
    : impl(std::make_unique<Impl>(std::move(config)))
{
}

DashboardApiServer::DashboardApiServer(Config config, const AuthManager& authManager,
    TransactionEngine& transactionEngine,
    const paybridge::terminal::TerminalManager& terminalManager,
    const paybridge::assets::AssetRegistry& assetRegistry)
    : impl(std::make_unique<Impl>(std::move(config), authManager, &transactionEngine,
        &terminalManager, &assetRegistry))
{
}

DashboardApiServer::DashboardApiServer(Config config, const AuthManager& authManager,
    TransactionEngine& transactionEngine,
    const paybridge::terminal::TerminalManager& terminalManager,
    const paybridge::assets::AssetRegistry& assetRegistry,
    PaymentOrchestrationService& paymentService)
    : impl(std::make_unique<Impl>(std::move(config), authManager, &transactionEngine,
        &terminalManager, &assetRegistry, &paymentService))
{
}

DashboardApiServer::~DashboardApiServer() = default;

bool DashboardApiServer::start() { return impl->start(); }
void DashboardApiServer::stop() noexcept { impl->requestStop(); }
void DashboardApiServer::join() noexcept { impl->join(); }
bool DashboardApiServer::isRunning() const noexcept { return impl->isRunning(); }
std::uint16_t DashboardApiServer::boundPort() const noexcept { return impl->boundPort(); }
