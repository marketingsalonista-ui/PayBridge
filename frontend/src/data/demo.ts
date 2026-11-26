// Static placeholders are retained only for the clearly-labelled sample logs
// module. Transactions, terminals, and assets always come from the API.
export const logs = [
  { time: '10:42:18', level: 'Info', event: 'Sample simulated status refresh', source: 'Dashboard' },
  { time: '10:39:55', level: 'Info', event: 'Sample simulated receipt event', source: 'Protocol' },
  { time: '10:35:12', level: 'Warning', event: 'Sample simulated decline response', source: 'Adapter' },
  { time: '10:28:09', level: 'Error', event: 'Sample simulated transmission failure', source: 'Socket' },
]
