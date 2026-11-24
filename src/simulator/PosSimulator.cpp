#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cctype>

#include "../core/TransactionEngine.h"
#include "../core/TransactionLogger.h"
#include "../protocol/JsonHandler.h"
#include "../money/AmountParser.h"
#include "../assets/AssetRegistry.h"

int main()
{
    std::cout << "\n====================================\n";
    std::cout << "       PAYBRIDGE POS SIMULATOR      \n";
    std::cout << "====================================\n";

    std::string assetType;
    std::string assetCode;

    std::cout << "\nAsset type (FIAT/DIGITAL_ASSET): ";
    std::cin >> assetType;
    std::cout << "Asset code: ";
    std::cin >> assetCode;

    for (char& character : assetCode)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));

    TransactionEngine engine;
    paybridge::assets::Asset asset;

    if (assetType == "FIAT")
    {
        const auto registered = engine.assets().findFiat(assetCode);
        if (!registered)
        {
            std::cout << "[ERROR] Unsupported fiat asset.\n";
            return 1;
        }
        asset = *registered;
    }
    else if (assetType == "DIGITAL_ASSET")
    {
        std::string network;
        std::string contractAddress;

        std::cout << "Network: ";
        std::cin >> network;
        std::cout << "Contract address: ";
        std::cin >> contractAddress;

        const auto registered = engine.assets().resolveDigitalAsset(
            assetCode,
            network,
            contractAddress
        );

        if (!registered)
        {
            std::cout << "[ERROR] Unsupported or untrusted digital asset identity.\n";
            return 1;
        }
        asset = *registered;
    }
    else
    {
        std::cout << "[ERROR] Invalid asset type.\n";
        return 1;
    }

    std::string amountText;
    std::cout << "Amount (decimal): ";
    std::cin >> amountText;

    const auto parsed = paybridge::money::parseAmount(
        amountText,
        asset.decimalPrecision
    );

    if (!parsed.success())
    {
        std::cout << "[ERROR] Invalid exact monetary amount.\n";
        return 1;
    }

    std::string externalReference;
    std::cout << "External reference: ";
    std::cin >> externalReference;

    if (externalReference.empty())
    {
        std::cout << "[ERROR] External reference is required.\n";
        return 1;
    }

    Transaction transaction = engine.createTransaction(
        parsed.money,
        asset,
        externalReference
    );

    if (transaction.status != TransactionStatus::CREATED)
    {
        std::cout << "[ERROR] Transaction creation failed.\n";
        return 1;
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        return 1;
    }

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET)
    {
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        WSACleanup();
        std::cout << "[ERROR] Socket creation failed.\n";
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(5050);
    inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr);

    if (connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == SOCKET_ERROR)
    {
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        closesocket(clientSocket);
        WSACleanup();
        std::cout << "[ERROR] Server connection failed.\n";
        return 1;
    }

    DWORD timeout = 10000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string request = JsonHandler::createPaymentRequest(
        transaction.transactionId,
        externalReference,
        asset,
        parsed.money
    );
    request += "\n";

    std::size_t sent = 0;
    while (sent < request.size())
    {
        const int result = send(
            clientSocket,
            request.c_str() + sent,
            static_cast<int>(request.size() - sent),
            0
        );

        if (result == SOCKET_ERROR || result == 0)
        {
            engine.updateStatus(transaction, TransactionStatus::FAILED);
            closesocket(clientSocket);
            WSACleanup();
            std::cout << "[ERROR] Request transmission failed.\n";
            return 1;
        }

        sent += static_cast<std::size_t>(result);
    }

    engine.updateStatus(transaction, TransactionStatus::SENT);

    std::string response;
    char buffer[1024];
    bool complete = false;

    while (response.size() <= 65536)
    {
        const int received = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;

        response.append(buffer, received);
        const std::size_t newline = response.find('\n');
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
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        std::cout << "[ERROR] Incomplete or missing response.\n";
        return 1;
    }

    try
    {
        const auto data = JsonHandler::parseMessage(response);

        if (!data.is_object() ||
            data.value("message_type", "") != "PAYMENT_RESPONSE" ||
            data.value("status", "") == "")
        {
            engine.updateStatus(transaction, TransactionStatus::FAILED);
            std::cout << "[ERROR] Invalid payment response.\n";
            return 1;
        }

        std::cout << "\nPAYBRIDGE RESPONSE:\n";
        std::cout << data.dump(4) << "\n";

        if (data["status"] == "RECEIVED")
        {
            engine.updateStatus(transaction, TransactionStatus::RECEIVED);
            std::cout << "[CONFIRMED] Server received the request.\n";
        }
        else
        {
            engine.updateStatus(transaction, TransactionStatus::FAILED);
            std::cout << "[FAILED] Server rejected the request.\n";
        }
    }
    catch (...)
    {
        engine.updateStatus(transaction, TransactionStatus::FAILED);
        std::cout << "[ERROR] Could not parse server response.\n";
    }

    std::cout << "\n========== TRANSACTION SUMMARY ==========" << "\n";
    std::cout << "Transaction ID : " << transaction.transactionId << "\n";
    std::cout << "External Ref   : " << externalReference << "\n";
    std::cout << "Asset          : " << asset.identityKey() << "\n";
    std::cout << "Amount         : "
              << paybridge::money::formatAmount(parsed.money) << "\n";
    std::cout << "Final Status   : "
              << TransactionEngine::statusToString(transaction.status) << "\n";
    std::cout << "=========================================\n";
    std::cout << "Note: Receipt confirmed, not payment approval.\n";

    return 0;
}




