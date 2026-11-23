cmake_minimum_required(VERSION 3.20)

project(PayBridge VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)

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

target_link_libraries(PayBridge
    PRIVATE nlohmann_json::nlohmann_json ws2_32 ole32
)

target_link_libraries(PayBridgeSimulator
    PRIVATE nlohmann_json::nlohmann_json ws2_32 ole32
)

