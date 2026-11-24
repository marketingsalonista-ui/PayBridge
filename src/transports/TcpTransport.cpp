#include "TcpTransport.h"

#include <cstring>
#include <utility>

namespace paybridge::transport
{

TcpTransport::TcpTransport(std::string host, std::uint16_t port)
    : host(std::move(host)), port(port)
{
}

TcpTransport::~TcpTransport()
{
    close();
}

TransportResult TcpTransport::open()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (socketHandle != INVALID_SOCKET)
        return {false, TransportError::ALREADY_OPEN, "TCP transport is already open"};

    if (host.empty() || port == 0)
        return {false, TransportError::INVALID_ARGUMENT, "TCP host or port is invalid"};

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return {false, TransportError::PLATFORM_ERROR, "Winsock initialization failed"};
    wsaInitialized = true;

    SOCKET socketHandleValue = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandleValue == INVALID_SOCKET)
    {
        WSACleanup();
        wsaInitialized = false;
        return {false, TransportError::PLATFORM_ERROR, "TCP socket creation failed"};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
    {
        closesocket(socketHandleValue);
        WSACleanup();
        wsaInitialized = false;
        return {false, TransportError::INVALID_ARGUMENT, "TCP host must be an IPv4 address"};
    }

    DWORD receiveTimeout = receiveTimeoutMilliseconds;
    DWORD sendTimeout = sendTimeoutMilliseconds;

    if (setsockopt(socketHandleValue, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout)) == SOCKET_ERROR ||
        setsockopt(socketHandleValue, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout)) == SOCKET_ERROR)
    {
        closesocket(socketHandleValue);
        WSACleanup();
        wsaInitialized = false;
        return {false, TransportError::PLATFORM_ERROR, "TCP timeout configuration failed"};
    }

    if (connect(socketHandleValue,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(socketHandleValue);
        WSACleanup();
        wsaInitialized = false;
        return {false, TransportError::CONNECT_FAILED,
                "TCP connect failed: " + std::to_string(error)};
    }

    socketHandle = socketHandleValue;
    return {true, TransportError::NONE, "TCP transport connected"};
}

void TcpTransport::close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);

    if (socketHandle != INVALID_SOCKET)
    {
        shutdown(socketHandle, SD_BOTH);
        closesocket(socketHandle);
        socketHandle = INVALID_SOCKET;
    }

    if (wsaInitialized)
    {
        WSACleanup();
        wsaInitialized = false;
    }
}

bool TcpTransport::isOpen() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    return socketHandle != INVALID_SOCKET;
}

TransportResult TcpTransport::send(const std::vector<std::uint8_t>& bytes)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (socketHandle == INVALID_SOCKET)
        return {false, TransportError::NOT_OPEN, "TCP transport is not open"};

    if (bytes.empty())
        return {false, TransportError::INVALID_ARGUMENT, "TCP send data is empty"};

    std::size_t offset = 0;

    while (offset < bytes.size())
    {
        const int remaining = static_cast<int>(bytes.size() - offset);
        const int sent = ::send(
            socketHandle,
            reinterpret_cast<const char*>(bytes.data()) + offset,
            remaining,
            0
        );

        if (sent > 0)
        {
            offset += static_cast<std::size_t>(sent);
            continue;
        }

        if (sent == 0)
            return {false, TransportError::DISCONNECTED, "TCP peer disconnected during send"};

        const int error = WSAGetLastError();
        if (error == WSAETIMEDOUT)
            return {false, TransportError::TIMEOUT, "TCP send timed out"};

        return {false, TransportError::PLATFORM_ERROR,
                "TCP send failed: " + std::to_string(error)};
    }

    return {true, TransportError::NONE, "TCP send completed"};
}

TransportBytes TcpTransport::receive(std::size_t maximumBytes)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (socketHandle == INVALID_SOCKET)
    {
        return {{false, TransportError::NOT_OPEN, "TCP transport is not open"}, {}};
    }

    if (maximumBytes == 0)
    {
        return {{false, TransportError::INVALID_ARGUMENT, "Maximum receive size is zero"}, {}};
    }

    std::vector<std::uint8_t> buffer(maximumBytes);

    const int received = recv(
        socketHandle,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(maximumBytes),
        0
    );

    if (received > 0)
    {
        buffer.resize(static_cast<std::size_t>(received));
        return {{true, TransportError::NONE, "TCP receive completed"}, std::move(buffer)};
    }

    if (received == 0)
        return {{false, TransportError::DISCONNECTED, "TCP peer disconnected"}, {}};

    const int error = WSAGetLastError();
    if (error == WSAETIMEDOUT)
        return {{false, TransportError::TIMEOUT, "TCP receive timed out"}, {}};

    return {{false, TransportError::PLATFORM_ERROR,
             "TCP receive failed: " + std::to_string(error)}, {}};
}

void TcpTransport::setReceiveTimeoutMilliseconds(std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    receiveTimeoutMilliseconds = milliseconds;

    if (socketHandle != INVALID_SOCKET)
    {
        DWORD timeout = milliseconds;
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }
}

void TcpTransport::setSendTimeoutMilliseconds(std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    sendTimeoutMilliseconds = milliseconds;

    if (socketHandle != INVALID_SOCKET)
    {
        DWORD timeout = milliseconds;
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }
}

std::string TcpTransport::description() const
{
    return "TCP " + host + ":" + std::to_string(port);
}

} // namespace paybridge::transport
