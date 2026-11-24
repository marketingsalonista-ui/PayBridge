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

# --------------------------------------------------
# PayBridge Server
# --------------------------------------------------

add_executable(PayBridge
    src/main.cpp

    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp

    src/protocol/JsonHandler.cpp

    src/communication/CommunicationManager.cpp

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
)

target_link_libraries(PayBridge
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        ws2_32
        ole32
)

# --------------------------------------------------
# POS Simulator
# --------------------------------------------------

add_executable(PayBridgeSimulator
    src/simulator/PosSimulator.cpp

    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp

    src/protocol/JsonHandler.cpp

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
)

target_link_libraries(PayBridgeSimulator
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        ws2_32
        ole32
)

# --------------------------------------------------
# Transaction Tests
# --------------------------------------------------

add_executable(TransactionEngineTests
    tests/TransactionEngineTests.cpp

    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp

    src/protocol/JsonHandler.cpp

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
)

target_link_libraries(TransactionEngineTests
    PRIVATE
        Threads::Threads
        ole32
        ws2_32
)

# --------------------------------------------------
# Testing
# --------------------------------------------------

enable_testing()

add_test(
    NAME TransactionEngineTests
    COMMAND TransactionEngineTests
)
