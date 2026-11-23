#include <iostream>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "../src/core/TransactionEngine.h"
#include "../src/protocol/JsonHandler.h"

int main()
{
    std::cout << "\n========== PAYBRIDGE POS SIMULATOR ==========\n";

    // Create a simulated transaction
    TransactionEngine engine;

    Transaction transaction = engine.createTransaction(12500, "AED");

    if (transaction.status != TransactionStatus::CREATED)
    {
        std::cerr << "[ERROR] Transaction creation failed.\n";
        return 1;
    }

    std::cout << "[CREATED] Transaction: "
              << transaction.transactionId << "\n";

    std::cout << "[AMOUNT] 125.00 AED\n";

    // Convert the transaction into a JSON request
    std::string request = JsonHandler::createPaymentRequest(
        transaction.transactionId,
        transaction.amountMinor,
        transaction.currency
    );

    request += "\n";

    // Initialize Windows networking
    WSADATA wsaData{};

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "[ERROR] Winsock initialization failed.\n";
        return 1;
    }

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (clientSocket == INVALID_SOCKET)
    {
        std::cerr << "[ERROR] Socket creation failed.\n";
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
        sizeof(serverAddress)) == SOCKET_ERROR)
    {
        std::cerr << "[ERROR] Could not connect to PayBridge server.\n";
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "[CONNECTED] Server connection established.\n";

    // Send the payment request
    int sent = 0;

    while (sent < static_cast<int>(request.size()))
    {
        int result = send(
            clientSocket,
            request.c_str() + sent,
            static_cast<int>(request.size()) - sent,
            0
        );

        if (result <= 0)
        {
            std::cerr << "[ERROR] Failed to send request.\n";
            closesocket(clientSocket);
            WSACleanup();
            return 1;
        }

        sent += result;
    }

    std::cout << "[SENT] Payment request transmitted.\n";

    // Receive the server response
    std::string response;
    char buffer[1024];

    while (response.size() <= 65536)
    {
        int received = recv(clientSocket, buffer, sizeof(buffer), 0);

        if (received <= 0)
            break;

        response.append(buffer, received);

        if (response.find('\n') != std::string::npos)
            break;
    }

    std::cout << "\nPAYBRIDGE SERVER RESPONSE:\n";

    if (!response.empty())
        std::cout << response << "\n";
    else
        std::cout << "[ERROR] No response received.\n";

    closesocket(clientSocket);
    WSACleanup();

    std::cout << "\n[COMPLETED] Simulator session ended.\n";

    return response.empty() ? 1 : 0;
}
