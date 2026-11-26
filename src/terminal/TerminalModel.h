#pragma once

#include <string>

namespace paybridge::terminal
{

enum class TransportKind
{
    TCP,
    SERIAL,
    SIMULATED
};

enum class TerminalConnectionStatus
{
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    UNKNOWN,
    FAILED
};

struct TerminalModel
{
    std::string terminalId;
    std::string manufacturer = "Verifone";
    std::string model = "VX 680";
    std::string description;
    // Empty selects the registered adapter for the transport kind; explicit
    // IDs permit multiple vendor-neutral adapter profiles in the future.
    std::string adapterId;
    TransportKind transportKind = TransportKind::SIMULATED;
    TerminalConnectionStatus connectionStatus = TerminalConnectionStatus::DISCONNECTED;
};

std::string transportKindToString(TransportKind transportKind);
std::string terminalConnectionStatusToString(TerminalConnectionStatus status);

} // namespace paybridge::terminal
