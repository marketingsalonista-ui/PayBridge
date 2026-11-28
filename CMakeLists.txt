cmake_minimum_required(VERSION 3.20)

project(PayBridge VERSION 0.6.0 LANGUAGES CXX)

# --------------------------------------------------
# C++ Configuration
# --------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------
# Dependencies
# --------------------------------------------------

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Boost 1.92 CONFIG REQUIRED)

# --------------------------------------------------
# PayBridge Server
# --------------------------------------------------

add_executable(PayBridge
    src/main.cpp

    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
    src/core/TransactionRepository.cpp
    src/core/PaymentOrchestrationService.cpp

    src/protocol/JsonHandler.cpp

    src/communication/CommunicationManager.cpp
    src/communication/DashboardApiServer.cpp
    src/security/AuthManager.cpp

    src/assets/Asset.cpp
    src/assets/AssetRegistry.cpp

    src/money/Money.cpp
    src/money/AmountParser.cpp

    src/adapters/FiatAdapter.cpp
    src/adapters/DigitalAssetAdapter.cpp

    src/transports/TransportResult.cpp
    src/transports/TcpTransport.cpp
    src/transports/SerialTransport.cpp

    src/terminal/TerminalModel.cpp
    src/terminal/TerminalManager.cpp
    src/terminal/SimulatedTerminalTransport.cpp
    src/terminal/SimulatedTerminalProtocol.cpp
    src/terminal/SimulatedTerminalPeer.cpp
    src/terminal/SimulatedTerminalAdapter.cpp
    src/terminal/VerifoneVx680SimulatorAdapter.cpp
    src/terminal/Vx680UnverifiedAdapter.cpp
)

target_link_libraries(PayBridge
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
        Boost::headers
        ws2_32.lib
        mswsock.lib
        ole32.lib
        advapi32.lib
)

# --------------------------------------------------
# POS Simulator
# --------------------------------------------------

add_executable(PayBridgeSimulator
    src/simulator/PosSimulator.cpp

    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
    src/core/TransactionRepository.cpp

    src/protocol/JsonHandler.cpp
    src/security/AuthManager.cpp

    src/assets/Asset.cpp
    src/assets/AssetRegistry.cpp

    src/money/Money.cpp
    src/money/AmountParser.cpp

    src/transports/TransportResult.cpp
    src/transports/TcpTransport.cpp
    src/transports/SerialTransport.cpp

    src/terminal/TerminalModel.cpp
    src/terminal/TerminalManager.cpp
    src/terminal/SimulatedTerminalTransport.cpp
    src/terminal/SimulatedTerminalProtocol.cpp
    src/terminal/SimulatedTerminalPeer.cpp
    src/terminal/SimulatedTerminalAdapter.cpp
)

target_link_libraries(PayBridgeSimulator
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
        ws2_32.lib
        ole32.lib
        advapi32.lib
)

# --------------------------------------------------
# Transaction Tests
# --------------------------------------------------

add_executable(TransactionEngineTests
    tests/TransactionEngineTests.cpp

    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
    src/core/TransactionRepository.cpp
    src/core/PaymentOrchestrationService.cpp

    src/protocol/JsonHandler.cpp

    src/communication/CommunicationManager.cpp
    src/security/AuthManager.cpp

    src/adapters/FiatAdapter.cpp
    src/adapters/DigitalAssetAdapter.cpp

    src/assets/Asset.cpp
    src/assets/AssetRegistry.cpp

    src/money/Money.cpp
    src/money/AmountParser.cpp

    src/transports/TransportResult.cpp
    src/transports/TcpTransport.cpp
    src/transports/SerialTransport.cpp

    src/terminal/TerminalModel.cpp
    src/terminal/TerminalManager.cpp
    src/terminal/SimulatedTerminalTransport.cpp
    src/terminal/SimulatedTerminalProtocol.cpp
    src/terminal/SimulatedTerminalPeer.cpp
    src/terminal/SimulatedTerminalAdapter.cpp
    src/terminal/VerifoneVx680SimulatorAdapter.cpp
    src/terminal/Vx680UnverifiedAdapter.cpp
)

target_link_libraries(TransactionEngineTests
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
        ws2_32.lib
        ole32.lib
        advapi32.lib
)

enable_testing()
add_test(NAME TransactionEngineTests COMMAND TransactionEngineTests)

add_executable(TerminalAdapterTests
    tests/TerminalAdapterTests.cpp
    src/assets/Asset.cpp
    src/money/Money.cpp
    src/money/AmountParser.cpp
    src/transports/TransportResult.cpp
    src/terminal/TerminalModel.cpp
    src/terminal/TerminalManager.cpp
    src/terminal/SimulatedTerminalTransport.cpp
    src/terminal/SimulatedTerminalProtocol.cpp
    src/terminal/SimulatedTerminalPeer.cpp
    src/terminal/SimulatedTerminalAdapter.cpp
    src/terminal/VerifoneVx680SimulatorAdapter.cpp
    src/terminal/Vx680UnverifiedAdapter.cpp
)

target_link_libraries(TerminalAdapterTests
    PRIVATE
        Threads::Threads
)

add_test(NAME TerminalAdapterTests COMMAND TerminalAdapterTests)

add_executable(DashboardApiServerTests
    tests/DashboardApiServerTests.cpp
    src/core/PaymentOrchestrationService.cpp
    src/protocol/JsonHandler.cpp
    src/communication/DashboardApiServer.cpp
    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
    src/core/TransactionRepository.cpp
    src/assets/Asset.cpp
    src/assets/AssetRegistry.cpp
    src/money/Money.cpp
    src/money/AmountParser.cpp
    src/security/AuthManager.cpp
    src/terminal/TerminalModel.cpp
    src/terminal/TerminalManager.cpp
    src/terminal/SimulatedTerminalTransport.cpp
    src/terminal/SimulatedTerminalProtocol.cpp
    src/terminal/SimulatedTerminalPeer.cpp
    src/terminal/SimulatedTerminalAdapter.cpp
    src/terminal/VerifoneVx680SimulatorAdapter.cpp
    src/terminal/Vx680UnverifiedAdapter.cpp
    src/transports/TransportResult.cpp
)

target_link_libraries(DashboardApiServerTests
    PRIVATE
        Boost::headers
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
        ws2_32.lib
        mswsock.lib
        ole32.lib
        advapi32.lib
)

add_test(NAME DashboardApiServerTests COMMAND DashboardApiServerTests)

add_executable(TcpServerLifecycleTests
    tests/TcpServerLifecycleTests.cpp
    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
    src/core/TransactionRepository.cpp
    src/core/PaymentOrchestrationService.cpp
    src/protocol/JsonHandler.cpp
    src/communication/CommunicationManager.cpp
    src/security/AuthManager.cpp
    src/adapters/FiatAdapter.cpp
    src/adapters/DigitalAssetAdapter.cpp
    src/assets/Asset.cpp
    src/assets/AssetRegistry.cpp
    src/money/Money.cpp
    src/money/AmountParser.cpp
    src/transports/TransportResult.cpp
    src/transports/TcpTransport.cpp
    src/transports/SerialTransport.cpp
    src/terminal/TerminalModel.cpp
    src/terminal/TerminalManager.cpp
    src/terminal/SimulatedTerminalTransport.cpp
    src/terminal/SimulatedTerminalProtocol.cpp
    src/terminal/SimulatedTerminalPeer.cpp
    src/terminal/SimulatedTerminalAdapter.cpp
    src/terminal/VerifoneVx680SimulatorAdapter.cpp
    src/terminal/Vx680UnverifiedAdapter.cpp
)

target_link_libraries(TcpServerLifecycleTests
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
        ws2_32.lib
        ole32.lib
        advapi32.lib
)

add_test(NAME TcpServerLifecycleTests COMMAND TcpServerLifecycleTests)

add_executable(Vx680UnverifiedAdapterTests
    tests/Vx680UnverifiedAdapterTests.cpp
    src/terminal/Vx680UnverifiedAdapter.cpp
    src/terminal/TerminalModel.cpp
    src/assets/Asset.cpp
    src/money/Money.cpp
    src/money/AmountParser.cpp
    src/transports/TransportResult.cpp
)

target_link_libraries(Vx680UnverifiedAdapterTests
    PRIVATE
        Threads::Threads
)

add_test(NAME Vx680UnverifiedAdapterTests COMMAND Vx680UnverifiedAdapterTests)

# --------------------------------------------------
# Standalone Verifone VX 680 Simulator Demo
# --------------------------------------------------

add_executable(VerifoneVx680Demo
    src/simulator/VerifoneVx680Demo.cpp

    src/assets/Asset.cpp

    src/money/Money.cpp
    src/money/AmountParser.cpp

    src/transports/TransportResult.cpp

    src/terminal/TerminalModel.cpp
    src/terminal/SimulatedTerminalTransport.cpp
    src/terminal/SimulatedTerminalProtocol.cpp
    src/terminal/SimulatedTerminalPeer.cpp
    src/terminal/SimulatedTerminalAdapter.cpp
    src/terminal/VerifoneVx680SimulatorAdapter.cpp
)

target_link_libraries(VerifoneVx680Demo
    PRIVATE
        Threads::Threads
)
