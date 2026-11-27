#include "CommunicationManager.h"
#include "../protocol/JsonHandler.h"
#include "../terminal/SimulatedTerminalTransport.h"
#include "../terminal/SimulatedTerminalAdapter.h"
#include "../terminal/Vx680UnverifiedAdapter.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iostream>
#include <string>
#include <exception>

#pragma comment(lib, "ws2_32.lib")

namespace
{
void configureDefaultSimulatedTerminal(paybridge::terminal::TerminalManager& manager)
{
    paybridge::terminal::TerminalModel terminal;
    terminal.terminalId = "SIM-DEFAULT-001";
    terminal.manufacturer = "PayBridge";
    terminal.model = "Simulated Terminal";
    terminal.description = "Protocol-neutral simulator; no real payment processing";
    terminal.transportKind = paybridge::terminal::TransportKind::SIMULATED;
    terminal.adapterId = paybridge::terminal::SimulatedTerminalAdapter::ID;
    manager.registerAdapter(std::make_shared<paybridge::terminal::SimulatedTerminalAdapter>());
    manager.registerAdapter(std::make_shared<paybridge::terminal::Vx680UnverifiedAdapter>());
    manager.registerTerminal(terminal);
    manager.setTransport(terminal.terminalId,
        std::make_unique<paybridge::terminal::SimulatedTerminalTransport>(
            paybridge::terminal::SimulatedTerminalOutcome::APPROVED));
}
}

CommunicationManager::CommunicationManager(TransactionEngine& engine)
    : engine(engine), authManager(AuthManager::fromEnvironment()),
      orchestrationService(engine, terminalManager)
{
    configureDefaultSimulatedTerminal(terminalManager);
}

CommunicationManager::CommunicationManager(
    TransactionEngine& engine,
    const AuthManager& authManager)
    : engine(engine), authManager(authManager),
      orchestrationService(engine, terminalManager)
{
    configureDefaultSimulatedTerminal(terminalManager);
}

nlohmann::json CommunicationManager::handleRequest(const std::string& request) const
{
    nlohmann::json response;
    response["message_type"] = "PAYMENT_RESPONSE";
    response["status"] = "REJECTED";

    std::string authorizationHeader;
    std::string payload = request;

    const std::size_t lf = request.find('\n');
    const std::size_t cr = request.find('\r');
    const bool crlf = cr != std::string::npos && lf == cr + 1;

    if (lf != std::string::npos || cr != std::string::npos)
    {
        const std::size_t lineEnd = crlf ? cr : (lf != std::string::npos ? lf : cr);
        const std::string firstLine = request.substr(0, lineEnd);

        if (firstLine.rfind("Authorization:", 0) == 0)
        {
            authorizationHeader = firstLine;
            if (crlf)
                authorizationHeader += '\r';

            const std::size_t payloadStart = crlf ? lf + 1 : lineEnd + 1;
            payload = payloadStart < request.size() ? request.substr(payloadStart) : std::string{};
            if (!payload.empty() && payload.back() == '\n') payload.pop_back();
            if (!payload.empty() && payload.back() == '\r') payload.pop_back();
        }
    }

    try
    {
        const nlohmann::json health = nlohmann::json::parse(payload);
        if (health.is_object() && health.value("message_type", "") == "HEALTH_REQUEST")
        {
            return nlohmann::json{
                {"message_type", "HEALTH_RESPONSE"},
                {"status", "OK"},
                {"sensitive", false}
            };
        }
    }
    catch (...) {}

    const auto verification = authManager.verifyAuthorization(authorizationHeader);
    if (verification == AuthManager::VerificationResult::MISSING)
    {
        response["error_code"] = "AUTH_MISSING";
        response["message"] = "Authentication credentials are required";
        return response;
    }
    if (verification == AuthManager::VerificationResult::MALFORMED)
    {
        response["error_code"] = "AUTH_MALFORMED";
        response["message"] = "Authentication credentials are malformed";
        return response;
    }
    if (verification == AuthManager::VerificationResult::INVALID)
    {
        response["error_code"] = "AUTH_INVALID";
        response["message"] = "Authentication credentials are invalid";
        return response;
    }
    if (verification != AuthManager::VerificationResult::ALLOWED)
    {
        response["error_code"] = "AUTH_UNAVAILABLE";
        response["message"] = "Authentication is unavailable";
        return response;
    }

    const auto validation = JsonHandler::parseAndValidatePaymentRequest(payload, engine.assets());
    if (!validation.valid)
    {
        response["error_code"] = validation.errorCode;
        response["message"] = validation.message;
        return response;
    }

    const PaymentOrchestrationResult operation = orchestrationService.process(
        validation.request, validation.request.terminalId);
    response["request_id"] = validation.request.envelope.requestId;
    response["status"] = operation.status;
    response["transaction_id"] = operation.transactionId;
    response["external_reference"] = operation.externalReference;
    response["message"] = operation.message;
    response["simulated"] = operation.simulated;
    if (!operation.terminalId.empty()) response["terminal_id"] = operation.terminalId;
    if (!operation.resultCode.empty()) response["result_code"] = operation.resultCode;
    if (!operation.errorCode.empty()) response["error_code"] = operation.errorCode;
    return response;
}

const paybridge::terminal::TerminalManager& CommunicationManager::terminalManagerReadOnly() const noexcept
{
    return terminalManager;
}

int CommunicationManager::runServer(unsigned short port,
    const std::atomic_bool* shutdownRequested, std::atomic_ushort* boundPort)
{
    if (!authManager.isConfigured())
    {
        std::cerr << "[ERROR] Authentication is not configured."
                  << std::endl;
        return 1;
    }

    WSADATA wsaData{};

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "[ERROR] Winsock initialization failed.\n";
        return 1;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (serverSocket == INVALID_SOCKET)
    {
        std::cerr << "[ERROR] Socket creation failed.\n";
        WSACleanup();
        return 1;
    }

    int reuseAddress = 1;
    setsockopt(
        serverSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddress),
        sizeof(reuseAddress)
    );

    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr);

    if (bind(
        serverSocket,
        reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)) == SOCKET_ERROR)
    {
        std::cerr << "[ERROR] Bind failed: "
                  << WSAGetLastError() << "\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    if (listen(serverSocket, 10) == SOCKET_ERROR)
    {
        std::cerr << "[ERROR] Listen failed.\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    sockaddr_in listeningAddress{};
    int listeningAddressLength = sizeof(listeningAddress);
    if (getsockname(serverSocket, reinterpret_cast<sockaddr*>(&listeningAddress),
            &listeningAddressLength) == 0 && boundPort)
        boundPort->store(ntohs(listeningAddress.sin_port));

    std::cout << "\n====================================\n";
    std::cout << "       PAYBRIDGE TCP SERVER\n";
    std::cout << "====================================\n";
    std::cout << "[READY] Listening on 127.0.0.1:" << port << "\n";
    std::cout << "[MODE] Multiple sequential clients\n";
    std::cout << "[INFO] Press Ctrl+C to stop the server.\n";

    unsigned long requestCounter = 0;

    while (!shutdownRequested || !shutdownRequested->load())
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(serverSocket, &readable);
        timeval readinessTimeout{};
        readinessTimeout.tv_sec = 0;
        readinessTimeout.tv_usec = 250000;
        const int ready = select(0, &readable, nullptr, nullptr, &readinessTimeout);
        if (ready == 0)
            continue;
        if (ready == SOCKET_ERROR)
        {
            if (shutdownRequested && shutdownRequested->load())
                break;
            std::cerr << "[ERROR] Listener readiness check failed: "
                      << WSAGetLastError() << "\n";
            continue;
        }

        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);

        if (clientSocket == INVALID_SOCKET)
        {
            if (shutdownRequested && shutdownRequested->load())
                break;
            std::cerr << "[ERROR] Accept failed: "
                      << WSAGetLastError() << "\n";
            continue;
        }

        ++requestCounter;

        DWORD timeout = 10000;

        setsockopt(
            clientSocket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout),
            sizeof(timeout)
        );

        setsockopt(
            clientSocket,
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout),
            sizeof(timeout)
        );

        std::cout << "\n[CONNECTED] Client #" << requestCounter << "\n";

        std::string request;
        char buffer[1024];
        bool complete = false;
        bool tooLarge = false;

        constexpr std::size_t MAX_MESSAGE_SIZE = 65536;

        while (true)
        {
            int received = recv(clientSocket, buffer, sizeof(buffer), 0);
            if (received <= 0)
                break;

            request.append(buffer, received);

            if (request.size() > MAX_MESSAGE_SIZE)
            {
                tooLarge = true;
                break;
            }

            const bool hasHeader = request.rfind("Authorization:", 0) == 0;

            if (hasHeader)
            {
                const std::size_t firstNewline = request.find('\n');
                if (firstNewline != std::string::npos)
                {
                    const std::size_t secondNewline = request.find('\n', firstNewline + 1);
                    if (secondNewline != std::string::npos)
                    {
                        request.resize(secondNewline);
                        complete = true;
                        break;
                    }
                }
            }
            else
            {
                const std::size_t newline = request.find('\n');
                if (newline != std::string::npos)
                {
                    request.resize(newline);
                    complete = true;
                    break;
                }
            }
        }

        nlohmann::json response;
        response["message_type"] = "PAYMENT_RESPONSE";
        response["status"] = "REJECTED";

        if (tooLarge)
        {
            response["error_code"] = "REQUEST_TOO_LARGE";
            response["message"] = "Request exceeds maximum size";
        }
        else if (complete)
        {
            response = handleRequest(request);
        }

        std::string output = response.dump() + "\n";
        std::size_t sent = 0;

        while (sent < output.size())
        {
            int result = send(clientSocket, output.c_str() + sent,
                              static_cast<int>(output.size() - sent), 0);
            if (result == SOCKET_ERROR || result == 0)
            {
                std::cerr << "[ERROR] Response transmission failed\n";
                break;
            }
            sent += static_cast<std::size_t>(result);
        }

        closesocket(clientSocket);
        std::cout << "[READY] Waiting for next transaction...\n";
    }

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
