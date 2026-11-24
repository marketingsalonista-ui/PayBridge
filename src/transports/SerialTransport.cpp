#include "SerialTransport.h"
#include <utility>

namespace paybridge::transport
{

SerialTransport::SerialTransport(std::string portName, std::uint32_t baudRate)
    : portName(std::move(portName)), baudRate(baudRate)
{
}

SerialTransport::~SerialTransport()
{
    close();
}

TransportResult SerialTransport::open()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (handle != INVALID_HANDLE_VALUE)
        return {false, TransportError::ALREADY_OPEN, "Serial transport is already open"};

    if (portName.empty() || baudRate == 0)
        return {false, TransportError::INVALID_ARGUMENT, "Serial port or baud rate is invalid"};

    std::string deviceName = portName;
    if (deviceName.rfind("\\\\.\\", 0) != 0)
        deviceName = "\\\\.\\" + deviceName;

    HANDLE newHandle = CreateFileA(
        deviceName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (newHandle == INVALID_HANDLE_VALUE)
    {
        return {false, TransportError::CONNECT_FAILED,
                "Could not open serial port: " + std::to_string(GetLastError())};
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(newHandle, &dcb))
    {
        CloseHandle(newHandle);
        return {false, TransportError::PLATFORM_ERROR, "Could not read serial COM state"};
    }

    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(newHandle, &dcb))
    {
        CloseHandle(newHandle);
        return {false, TransportError::PLATFORM_ERROR, "Could not configure serial COM state"};
    }

    if (!SetCommMask(newHandle, EV_RXCHAR | EV_ERR))
    {
        CloseHandle(newHandle);
        return {false, TransportError::PLATFORM_ERROR, "Could not configure serial COM events"};
    }

    handle = newHandle;
    configureTimeOuts();

    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    return {true, TransportError::NONE, "Serial COM port opened"};
}

void SerialTransport::close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);

    if (handle != INVALID_HANDLE_VALUE)
    {
        PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
}

bool SerialTransport::isOpen() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    return handle != INVALID_HANDLE_VALUE;
}

void SerialTransport::configureTimeOuts()
{
    if (handle == INVALID_HANDLE_VALUE)
        return;

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutConstant = receiveTimeoutMilliseconds;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = sendTimeoutMilliseconds;
    timeouts.WriteTotalTimeoutMultiplier = 0;

    SetCommTimeouts(handle, &timeouts);
}

TransportResult SerialTransport::send(const std::vector<std::uint8_t>& bytes)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (handle == INVALID_HANDLE_VALUE)
        return {false, TransportError::NOT_OPEN, "Serial transport is not open"};

    if (bytes.empty())
        return {false, TransportError::INVALID_ARGUMENT, "Serial send data is empty"};

    std::size_t offset = 0;

    while (offset < bytes.size())
    {
        DWORD written = 0;
        const BOOL success = WriteFile(
            handle,
            bytes.data() + offset,
            static_cast<DWORD>(bytes.size() - offset),
            &written,
            nullptr
        );

        if (!success)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_TIMEOUT)
                return {false, TransportError::TIMEOUT, "Serial send timed out"};

            return {false, TransportError::PLATFORM_ERROR,
                    "Serial send failed: " + std::to_string(error)};
        }

        if (written == 0)
            return {false, TransportError::DISCONNECTED, "Serial write produced no progress"};

        offset += static_cast<std::size_t>(written);
    }

    return {true, TransportError::NONE, "Serial send completed"};
}

TransportBytes SerialTransport::receive(std::size_t maximumBytes)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return {{false, TransportError::NOT_OPEN, "Serial transport is not open"}, {}};
    }

    if (maximumBytes == 0)
    {
        return {{false, TransportError::INVALID_ARGUMENT, "Maximum receive size is zero"}, {}};
    }

    std::vector<std::uint8_t> buffer(maximumBytes);
    DWORD received = 0;

    const BOOL success = ReadFile(
        handle,
        buffer.data(),
        static_cast<DWORD>(maximumBytes),
        &received,
        nullptr
    );

    if (!success)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_TIMEOUT)
            return {{false, TransportError::TIMEOUT, "Serial receive timed out"}, {}};

        return {{false, TransportError::PLATFORM_ERROR,
                 "Serial receive failed: " + std::to_string(error)}, {}};
    }

    if (received == 0)
        return {{false, TransportError::TIMEOUT, "Serial receive timed out with no bytes"}, {}};

    buffer.resize(static_cast<std::size_t>(received));
    return {{true, TransportError::NONE, "Serial receive completed"}, std::move(buffer)};
}

void SerialTransport::setReceiveTimeoutMilliseconds(std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    receiveTimeoutMilliseconds = milliseconds;
    configureTimeOuts();
}

void SerialTransport::setSendTimeoutMilliseconds(std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    sendTimeoutMilliseconds = milliseconds;
    configureTimeOuts();
}

std::string SerialTransport::description() const
{
    return "SERIAL " + portName + " @ " + std::to_string(baudRate);
}

} // namespace paybridge::transport
