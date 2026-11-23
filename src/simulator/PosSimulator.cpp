#include <iostream>
#include <string>
#include <iomanip>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../core/TransactionEngine.h"
#include "../protocol/JsonHandler.h"
#include "../core/TransactionLogger.h"

int main()
{
    std::cout << "\n====================================\n";
    std::cout << "       PAYBRIDGE POS SIMULATOR      \n";
    std::cout << "====================================\n";

    double amount = 0;

    std::cout << "\nEnter payment amount (AED): ";
    std::cin >> amount;

    if (!std::cin || amount <= 0 || amount > 10000000)
    {
        std::cout << "[ERROR] Invalid amount.\n";
        return 1;
    }

    long long amountMinor =
        static_cast<long long>(amount * 100 + 0.5);

    TransactionEngine engine;

    Transaction transaction =
        engine.createTransaction(amountMinor, "AED");

    if (transaction.status != TransactionStatus::CREATED)
    {
        std::cout << "[ERROR] Transaction creation failed.\n";
        return 1;
    }

    auto showStatus = [&transaction]()
    {
        std::cout << "[STATUS] "
                  << TransactionEngine::statusToString(
                         transaction.status)
                  << "\n";
    };

    std::cout << "\n[CREATED] Transaction: "
              << transaction.transactionId << "\n";

    showStatus();

    WSADATA wsaData{};

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        showStatus();
        return 1;
    }

    SOCKET clientSocket =
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (clientSocket == INVALID_SOCKET)
    {
        std::cout << "[ERROR] Socket creation failed.\n";
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        showStatus();
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(5050);
    inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr);

    std::cout << "[CONNECTING] PayBridge server...\n";

    if (connect(
        clientSocket,
        reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)
    ) == SOCKET_ERROR)
    {
        std::cout << "[ERROR] Server connection failed.\n";
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        showStatus();
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "[CONNECTED] Successfully connected.\n";

    std::string request =
        JsonHandler::createPaymentRequest(
            transaction.transactionId,
            amountMinor,
            "AED"
        );

    request += "\n";

    int sent = 0;

    while (sent < static_cast<int>(request.size()))
    {
        int result = send(
            clientSocket,
            request.c_str() + sent,
            static_cast<int>(request.size()) - sent,
            0
        );

        if (result == SOCKET_ERROR || result == 0)
        {
            std::cout << "[ERROR] Request transmission failed.\n";
            engine.updateStatus(transaction, TransactionStatus::FAILED);
            showStatus();
            closesocket(clientSocket);
            WSACleanup();
            return 1;
        }

        sent += result;
    }

    engine.updateStatus(transaction, TransactionStatus::SENT);

    std::cout << "[SENT] Payment request transmitted.\n";
    showStatus();

    std::string response;
    char buffer[1024];
    bool complete = false;

    while (response.size() <= 65536)
    {
        int received = recv(
            clientSocket,
            buffer,
            sizeof(buffer),
            0
        );

        if (received <= 0)
            break;

        response.append(buffer, received);

        std::size_t newline = response.find('\n');

        if (newline != std::string::npos)
        {
            response.resize(newline);
            complete = true;
            break;
        }
    }

    closesocket(clientSocket);
    WSACleanup();

    if (!complete)
    {
        std::cout << "[ERROR] Incomplete or missing response.\n";
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        showStatus();
        return 1;
    }

    try
    {
        auto data = JsonHandler::parseMessage(response);

        bool validResponse =
            data.is_object()
            && data.value("message_type", "") == "PAYMENT_RESPONSE"
            && data.value("transaction_id", "") == transaction.transactionId
            && data.contains("status")
            && data["status"].is_string();

        if (!validResponse)
        {
            std::cout << "[ERROR] Invalid payment response.\n";
            engine.updateStatus(transaction, TransactionStatus::FAILED);
            showStatus();
            return 1;
        }

        std::cout << "\nPAYBRIDGE RESPONSE:\n";
        std::cout << data.dump(4) << "\n";

        std::string responseStatus = data["status"];

        if (responseStatus == "RECEIVED")
        {
            engine.updateStatus(
                transaction,
                TransactionStatus::RECEIVED
            );

            std::cout << "\n[CONFIRMED] Server received the request.\n";
        }
        else
        {
            engine.updateStatus(
                transaction,
                TransactionStatus::FAILED
            );

            std::cout << "\n[FAILED] Server rejected the request.\n";
        }
    }
    catch (...)
    {
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        std::cout << "[ERROR] Could not parse server response.\n";
    }
    TransactionLogger::logTransaction(transaction);
std::cout << "\n========== TRANSACTION SUMMARY ==========\n";

    std::cout << "Transaction ID : "
              << transaction.transactionId << "\n";

    std::cout << "Amount         : AED "
              << std::fixed << std::setprecision(2)
              << amountMinor / 100.0 << "\n";

    std::cout << "Currency       : AED\n";

    std::cout << "Final Status   : "
              << TransactionEngine::statusToString(
                     transaction.status)
              << "\n";

    std::cout << "=========================================\n";

    if (transaction.status == TransactionStatus::RECEIVED)
        std::cout << "Note: Receipt confirmed, not payment approval.\n";

    std::cout << "\n[COMPLETE] Simulator finished.\n";

    return 0;
}




