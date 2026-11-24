#include "CommunicationManager.h"
#include "../protocol/JsonHandler.h"
#include "../adapters/FiatAdapter.h"
#include "../adapters/DigitalAssetAdapter.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iostream>
#include <string>
#include <exception>

#pragma comment(lib, "ws2_32.lib")

CommunicationManager::CommunicationManager(TransactionEngine& engine)
    : engine(engine)
{
}

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
        else if (complete)
        {
            const auto validation = JsonHandler::parseAndValidatePaymentRequest(
                request,
                engine.assets()
            );

            if (!validation.valid)
            {
                response["error_code"] = validation.errorCode;
                response["message"] = validation.message;
                std::cout << "[REJECTED] " << validation.errorCode << "\n";
            }
            else
            {
                Transaction transaction = engine.createTransaction(
                    validation.request.money,
                    validation.request.asset,
                    validation.request.externalReference
                );

                if (validation.request.transactionId.empty() == false)
                {
                    response["submitted_transaction_id"] =
                        validation.request.transactionId;
                }

                if (transaction.status == TransactionStatus::INVALID ||
                    transaction.status == TransactionStatus::FAILED)
                {
                    response["error_code"] = "TRANSACTION_REJECTED";
                    response["message"] = "Transaction could not be created or retried safely";
                }
                else
                {
                    if (!validation.request.transactionId.empty() &&
                        validation.request.transactionId != transaction.transactionId)
                    {
                        response["error_code"] = "TRANSACTION_ID_MISMATCH";
                        response["message"] = "Submitted transaction_id conflicts with the authoritative transaction";
                    }
                    else
                    {
                        paybridge::adapters::FiatAdapter fiatAdapter;
                        paybridge::adapters::DigitalAssetAdapter digitalAdapter;
                        paybridge::adapters::PaymentAdapter* adapter = nullptr;

                        if (transaction.asset.isFiat())
                            adapter = &fiatAdapter;
                        else if (transaction.asset.isDigitalAsset())
                            adapter = &digitalAdapter;

                        if (adapter == nullptr || !adapter->supports(transaction.asset))
                        {
                            response["error_code"] = "UNSUPPORTED_ASSET";
                            response["message"] = "No supported payment adapter is configured for the asset";
                        }
                        else
                        {
                            if (transaction.status == TransactionStatus::RECEIVED ||
                                transaction.status == TransactionStatus::APPROVED ||
                                transaction.status == TransactionStatus::DECLINED)
                            {
                                response["status"] = "RECEIVED";
                                response["transaction_id"] = transaction.transactionId;
                                response["external_reference"] = transaction.externalReference;
                                response["message"] =
                                    "Existing transaction returned; no state transition applied";
                                response["existing_status"] =
                                    TransactionEngine::statusToString(transaction.status);

                                std::cout << "[DUPLICATE] Existing transaction returned\n";
                                std::cout << "[STATUS] "
                                          << TransactionEngine::statusToString(transaction.status)
                                          << "\n";
                            }
                            else
                            {
                                const auto adapterResult = adapter->simulate(
                                    transaction.asset,
                                    transaction.money,
                                    transaction.externalReference
                                );

                                if (!adapterResult.accepted)
                                {
                                    response["error_code"] = "ADAPTER_REJECTED";
                                    response["message"] = adapterResult.message;
                                    engine.updateStatus(transaction, TransactionStatus::FAILED);
                                }
                                else
                                {
                                    if (!engine.updateStatus(transaction, TransactionStatus::SENT) ||
                                        !engine.updateStatus(transaction, TransactionStatus::RECEIVED))
                                    {
                                        response["error_code"] = "PERSISTENCE_FAILURE";
                                        response["message"] = "Transaction state could not be persisted before receipt";
                                    }
                                    else
                                    {
                                        response["status"] = "RECEIVED";
                                        response["transaction_id"] = transaction.transactionId;
                                        response["external_reference"] = transaction.externalReference;
                                        response["message"] = adapterResult.message;

                                        std::cout << "[RECEIVED] Payment request\n";
                                        std::cout << "[TRANSACTION] "
                                                  << transaction.transactionId << "\n";
                                        std::cout << "[ASSET] "
                                                  << transaction.asset.identityKey() << "\n";
                                    }
                                }
                            }
                        }
                    }
                }
            }
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
