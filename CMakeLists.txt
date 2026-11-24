
cmake_minimum_required(VERSION 3.20)

project(PayBridge VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(PayBridge
    src/main.cpp
    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
    src/protocol/JsonHandler.cpp
    src/communication/CommunicationManager.cpp
)

add_executable(PayBridgeSimulator
    src/simulator/PosSimulator.cpp
    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
    src/protocol/JsonHandler.cpp
)

add_executable(TransactionEngineTests
    tests/TransactionEngineTests.cpp
    src/core/TransactionEngine.cpp
    src/core/TransactionLogger.cpp
)

target_link_libraries(PayBridge
    PRIVATE
    nlohmann_json::nlohmann_json
    ws2_32
    ole32
    Threads::Threads
)

target_link_libraries(PayBridgeSimulator
    PRIVATE
    nlohmann_json::nlohmann_json
    ws2_32
    ole32
    Threads::Threads
)

target_link_libraries(TransactionEngineTests
    PRIVATE
    ole32
    Threads::Threads
)

enable_testing()

add_test(
    NAME TransactionEngineTests
    COMMAND TransactionEngineTests
)
