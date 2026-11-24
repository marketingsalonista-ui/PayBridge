Write-Host "========== PAYBRIDGE SECURITY AUDIT =========="

Write-Host "`n[1] Firewall status"

Get-NetFirewallProfile |
    Format-Table Name, Enabled, DefaultInboundAction, DefaultOutboundAction

Write-Host "`n[2] Network profile"

Get-NetConnectionProfile |
    Format-Table InterfaceAlias, NetworkCategory, IPv4Connectivity

Write-Host "`n[3] PayBridge port 5050"

Get-NetTCPConnection -LocalPort 5050 -ErrorAction SilentlyContinue |
    Format-Table LocalAddress, LocalPort, State, OwningProcess

Write-Host "`n[4] PayBridge firewall rules"

Get-NetFirewallRule -ErrorAction SilentlyContinue |
    Where-Object DisplayName -Like "PayBridge*" |
    Format-Table DisplayName, Enabled, Direction, Action, Profile

Write-Host "`n========== AUDIT COMPLETE =========="
