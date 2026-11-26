#include "../src/core/TransactionEngine.h"
#include "../src/core/TransactionLogger.h"
#include "../src/communication/CommunicationManager.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("paybridge_tcp_lifecycle_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    TransactionEngine engine((root / "test.sqlite").string(),
        (root / "unused.csv").string());
    assert(engine.initialize());
    AuthManager auth = AuthManager::fromBearerToken(AuthManager::generateBearerToken());
    assert(auth.isConfigured());
    CommunicationManager communication(engine, auth);

    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    std::atomic_bool shutdown{false};
    std::atomic_ushort boundPort{0};
    std::atomic_int serverResult{-1};
    std::thread server([&]
    {
        serverResult.store(communication.runServer(0, &shutdown, &boundPort));
    });

    for (int attempt = 0; attempt < 100 && boundPort.load() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(boundPort.load() != 0);

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(boundPort.load());
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    assert(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    constexpr char request[] = "{\"message_type\":\"HEALTH_REQUEST\"}\n";
    assert(send(client, request, static_cast<int>(sizeof(request) - 1), 0) > 0);
    char response[1024]{};
    const int received = recv(client, response, sizeof(response) - 1, 0);
    assert(received > 0);
    const std::string responseText(response, static_cast<std::size_t>(received));
    assert(responseText.find("\"message_type\":\"HEALTH_RESPONSE\"") != std::string::npos);
    assert(responseText.find("\"status\":\"OK\"") != std::string::npos);
    closesocket(client);

    shutdown.store(true);
    server.join();
    assert(serverResult.load() == 0);
    WSACleanup();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "TCP payment listener health request and clean shutdown passed\n";
    return 0;
}
