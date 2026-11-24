export type ModuleName =
  | 'Overview'
  | 'Transactions'
  | 'POS Terminals'
  | 'Assets'
  | 'Adapters'
  | 'API Protocol'
  | 'Security'
  | 'Logs'
  | 'Settings'

export type TransactionStatus = 'RECEIVED' | 'APPROVED' | 'DECLINED' | 'FAILED'

export interface Transaction {
  id: string
  reference: string
  asset: string
  amount: string
  status: TransactionStatus
  terminal: string
  time: string
}

export const transactions: Transaction[] = [
  { id: 'PB-8F2A1C', reference: 'POS-2026-1048', asset: 'AED', amount: 'AED 2,450.00', status: 'APPROVED', terminal: 'Dubai Mall 01', time: '10:42:18' },
  { id: 'PB-7D19BE', reference: 'POS-2026-1047', asset: 'USD', amount: 'USD 840.50', status: 'RECEIVED', terminal: 'Terminal 04', time: '10:39:55' },
  { id: 'PB-3AE774', reference: 'POS-2026-1046', asset: 'USDT', amount: 'USDT 1,240.00', status: 'DECLINED', terminal: 'Digital Desk', time: '10:35:12' },
  { id: 'PB-91C0FD', reference: 'POS-2026-1045', asset: 'GBP', amount: 'GBP 620.20', status: 'APPROVED', terminal: 'London Hub', time: '10:31:44' },
  { id: 'PB-44B821', reference: 'POS-2026-1044', asset: 'EUR', amount: 'EUR 390.00', status: 'FAILED', terminal: 'Terminal 07', time: '10:28:09' },
  { id: 'PB-62DE15', reference: 'POS-2026-1043', asset: 'INR', amount: 'INR 18,500.00', status: 'APPROVED', terminal: 'Mumbai 02', time: '10:22:37' },
  { id: 'PB-20A631', reference: 'POS-2026-1042', asset: 'SAR', amount: 'SAR 1,900.00', status: 'RECEIVED', terminal: 'Riyadh 01', time: '10:18:20' },
]

export const volumeData = [
  { name: 'Mon', volume: 62, received: 18 },
  { name: 'Tue', volume: 78, received: 22 },
  { name: 'Wed', volume: 68, received: 16 },
  { name: 'Thu', volume: 91, received: 25 },
  { name: 'Fri', volume: 84, received: 20 },
  { name: 'Sat', volume: 106, received: 29 },
  { name: 'Sun', volume: 98, received: 24 },
]

export const statusData = [
  { name: 'Approved', value: 68, color: '#10b981' },
  { name: 'Received', value: 18, color: '#6366f1' },
  { name: 'Declined', value: 9, color: '#f59e0b' },
  { name: 'Failed', value: 5, color: '#ef4444' },
]

export const assets = [
  { code: 'AED', name: 'UAE Dirham', type: 'Fiat', precision: 2, status: 'Enabled' },
  { code: 'GBP', name: 'British Pound', type: 'Fiat', precision: 2, status: 'Enabled' },
  { code: 'USD', name: 'US Dollar', type: 'Fiat', precision: 2, status: 'Enabled' },
  { code: 'EUR', name: 'Euro', type: 'Fiat', precision: 2, status: 'Enabled' },
  { code: 'INR', name: 'Indian Rupee', type: 'Fiat', precision: 2, status: 'Enabled' },
  { code: 'SAR', name: 'Saudi Riyal', type: 'Fiat', precision: 2, status: 'Enabled' },
  { code: 'JPY', name: 'Japanese Yen', type: 'Fiat', precision: 0, status: 'Enabled' },
  { code: 'KWD', name: 'Kuwaiti Dinar', type: 'Fiat', precision: 3, status: 'Enabled' },
  { code: 'USDT', name: 'Tether', type: 'Digital Asset', precision: 6, status: 'Demo config' },
]

export const logs = [
  { time: '10:42:18', level: 'Info', event: 'Demo transaction state updated', source: 'Dashboard' },
  { time: '10:39:55', level: 'Info', event: 'Demo receipt payload received', source: 'Protocol' },
  { time: '10:35:12', level: 'Warning', event: 'Demo decline response displayed', source: 'Adapter' },
  { time: '10:28:09', level: 'Error', event: 'Demo transmission failure simulated', source: 'Socket' },
]
