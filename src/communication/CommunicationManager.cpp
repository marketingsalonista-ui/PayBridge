#include "CommunicationManager.h"
#include "../protocol/JsonHandler.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iostream>
#include <string>
#include <exception>

#pragma comment(lib, "ws2_32.lib")

int CommunicationManager::runServer(unsigned short port)
{
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

    std::cout << "\n====================================\n";
    std::cout << "       PAYBRIDGE TCP SERVER\n";
    std::cout << "====================================\n";
    std::cout << "[READY] Listening on 127.0.0.1:"
              << port << "\n";
    std::cout << "[MODE] Multiple sequential clients\n";
    std::cout << "[INFO] Press Ctrl+C to stop the server.\n";

    unsigned long requestCounter = 0;

    while (true)
    {
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);

        if (clientSocket == INVALID_SOCKET)
        {
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
            int received = recv(
                clientSocket,
                buffer,
                sizeof(buffer),
                0
            );

            if (received <= 0)
                break;

            request.append(buffer, received);

            std::size_t newline = request.find('\n');

            if (newline != std::string::npos)
            {
                request.resize(newline);
                complete = true;
                break;
            }

            if (request.size() > MAX_MESSAGE_SIZE)
            {
                tooLarge = true;
                break;
            }
        }

        nlohmann::json response;
        response["message_type"] = "PAYMENT_RESPONSE";
        response["status"] = "REJECTED";

        if (tooLarge)
        {
            response["message"] = "Request exceeds maximum size";
        }
        else if (complete && JsonHandler::validateMessage(request))
        {
            try
            {
                auto data = JsonHandler::parseMessage(request);

                bool validPayment =
                    data.value("message_type", "") == "PAYMENT_REQUEST"
                    && !data["transaction_id"].get<std::string>().empty()
                    && data.contains("payment")
                    && data["payment"].is_object()
                    && data["payment"].contains("amount_minor")
                    && data["payment"]["amount_minor"].is_number_integer()
                    && data["payment"]["amount_minor"].get<long long>() > 0
                    && data["payment"].contains("currency")
                    && data["payment"]["currency"].is_string()
                    && data["payment"]["currency"].get<std::string>().size() == 3;

                if (validPayment)
                {
                    std::string transactionId =
                        data["transaction_id"].get<std::string>();

                    response["status"] = "RECEIVED";
                    response["transaction_id"] = transactionId;
                    response["message"] = "Request received by PayBridge";

                    std::cout << "[RECEIVED] Payment request\n";
                    std::cout << "[TRANSACTION] "
                              << transactionId << "\n";
                    std::cout << "[AMOUNT MINOR] "
                              << data["payment"]["amount_minor"] << "\n";
                }
                else
                {
                    response["message"] = "Invalid payment request";
                    std::cout << "[REJECTED] Invalid payment data\n";
                }
            }
            catch (const std::exception& error)
            {
                response["message"] = "Request parsing failed";
                std::cerr << "[ERROR] " << error.what() << "\n";
            }
        }
        else
        {
            response["message"] = "Invalid or incomplete JSON message";
            std::cout << "[REJECTED] Invalid or incomplete request\n";
        }

        std::string output = response.dump() + "\n";
        std::size_t sent = 0;

        while (sent < output.size())
        {
            int result = send(
                clientSocket,
                output.c_str() + sent,
                static_cast<int>(output.size() - sent),
                0
            );

            if (result == SOCKET_ERROR || result == 0)
            {
                std::cerr << "[ERROR] Response transmission failed\n";
                break;
            }

            sent += static_cast<std::size_t>(result);
        }

        if (sent == output.size())
            std::cout << "[SENT] Response returned to client\n";

        closesocket(clientSocket);

        std::cout << "[CLOSED] Client connection closed\n";
        std::cout << "[READY] Waiting for next transaction...\n";
    }

    closesocket(serverSocket);
    WSACleanup();

    return 0;
}
