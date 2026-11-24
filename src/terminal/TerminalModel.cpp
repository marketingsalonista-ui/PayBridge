#include "TerminalModel.h"

namespace paybridge::terminal
{

std::string transportKindToString(TransportKind transportKind)
{
    switch (transportKind)
    {
        case TransportKind::TCP: return "TCP";
        case TransportKind::SERIAL: return "SERIAL";
        case TransportKind::SIMULATED: return "SIMULATED";
    }
    return "UNKNOWN";
}

std::string terminalConnectionStatusToString(TerminalConnectionStatus status)
{
    switch (status)
    {
        case TerminalConnectionStatus::DISCONNECTED: return "DISCONNECTED";
        case TerminalConnectionStatus::CONNECTING: return "CONNECTING";
        case TerminalConnectionStatus::CONNECTED: return "CONNECTED";
        case TerminalConnectionStatus::UNKNOWN: return "UNKNOWN";
        case TerminalConnectionStatus::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

} // namespace paybridge::terminal
