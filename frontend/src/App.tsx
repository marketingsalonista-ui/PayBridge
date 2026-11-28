import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  Activity,
  Boxes,
  ChevronRight,
  CircleGauge,
  Database,
  LayoutDashboard,
  LockKeyhole,
  Moon,
  Search,
  Settings,
  ShieldCheck,
  TerminalSquare,
  WalletCards,
  X,
} from 'lucide-react'
import { logs } from './data/demo'
import {
  payBridgeApi,
  PayBridgeApiError,
} from './services/api'
import type {
  AdapterDto,
  AssetDto,
  HealthResponse,
  TransactionDto,
  TransactionStatus,
  TransactionsResponse,
} from './services/api'

type ModuleName =
  | 'Overview'
  | 'Transactions'
  | 'POS Terminals'
  | 'Assets'
  | 'Adapters'
  | 'API Protocol'
  | 'Security'
  | 'Logs'
  | 'Settings'

interface ResourceState<T> {
  data: T | null
  loading: boolean
  refreshing: boolean
  error: string | null
  lastUpdated: Date | null
}

const navigation: Array<{ name: ModuleName; icon: typeof LayoutDashboard }> = [
  { name: 'Overview', icon: LayoutDashboard },
  { name: 'Transactions', icon: Activity },
  { name: 'POS Terminals', icon: TerminalSquare },
  { name: 'Assets', icon: WalletCards },
  { name: 'Adapters', icon: Boxes },
  { name: 'API Protocol', icon: Database },
]

const operations: Array<{ name: ModuleName; icon: typeof LayoutDashboard }> = [
  { name: 'Security', icon: ShieldCheck },
  { name: 'Logs', icon: CircleGauge },
  { name: 'Settings', icon: Settings },
]

function useApiResource<T>(
  loader: (signal: AbortSignal) => Promise<T>,
  pollingMilliseconds: number | null,
) {
  const [revision, setRevision] = useState(0)
  const [state, setState] = useState<ResourceState<T>>({
    data: null,
    loading: true,
    refreshing: false,
    error: null,
    lastUpdated: null,
  })

  useEffect(() => {
    let disposed = false
    let inFlight = false
    let controller: AbortController | null = null

    const load = async () => {
      if (disposed || inFlight) return
      inFlight = true
      controller = new AbortController()
      setState((previous) => ({
        ...previous,
        loading: previous.data === null,
        refreshing: previous.data !== null,
        error: null,
      }))
      try {
        const data = await loader(controller.signal)
        if (!disposed) {
          setState({ data, loading: false, refreshing: false, error: null, lastUpdated: new Date() })
        }
      } catch (error) {
        if (!disposed && !controller.signal.aborted) {
          const message = error instanceof PayBridgeApiError
            ? error.message
            : 'Dashboard data could not be loaded.'
          setState((previous) => ({ ...previous, loading: false, refreshing: false, error: message }))
        }
      } finally {
        inFlight = false
      }
    }

    void load()
    const timer = pollingMilliseconds === null
      ? null
      : window.setInterval(() => { void load() }, pollingMilliseconds)
    return () => {
      disposed = true
      if (timer !== null) window.clearInterval(timer)
      controller?.abort()
    }
  }, [loader, pollingMilliseconds, revision])

  return { ...state, retry: () => setRevision((value) => value + 1) }
}

function formatAmount(units: string, precision: number, code: string): string {
  const negative = units.startsWith('-')
  const digits = (negative ? units.slice(1) : units).replace(/^0+(?=\d)/, '')
  const safePrecision = Math.max(0, Math.min(18, precision))
  const padded = digits.padStart(safePrecision + 1, '0')
  const split = safePrecision === 0 ? padded.length : padded.length - safePrecision
  const whole = padded.slice(0, split).replace(/\B(?=(\d{3})+(?!\d))/g, ',')
  const fraction = safePrecision === 0 ? '' : `.${padded.slice(split)}`
  return `${code} ${negative ? '-' : ''}${whole}${fraction}`
}

function formatTimestamp(timestamp: number): string {
  return new Date(timestamp * 1000).toLocaleString()
}

function formatRefresh(timestamp: Date | null): string {
  return timestamp ? timestamp.toLocaleTimeString() : 'Waiting for first refresh'
}

function StatusBadge({ status }: { status: TransactionStatus }) {
  const styles: Record<TransactionStatus, string> = {
    CREATED: 'bg-slate-100 text-slate-700 dark:bg-slate-700/40 dark:text-slate-200',
    SENT: 'bg-blue-50 text-blue-700 dark:bg-blue-500/10 dark:text-blue-300',
    RECEIVED: 'bg-indigo-50 text-indigo-700 dark:bg-indigo-500/10 dark:text-indigo-300',
    APPROVED: 'bg-emerald-50 text-emerald-700 dark:bg-emerald-500/10 dark:text-emerald-300',
    DECLINED: 'bg-amber-50 text-amber-700 dark:bg-amber-500/10 dark:text-amber-300',
    FAILED: 'bg-red-50 text-red-700 dark:bg-red-500/10 dark:text-red-300',
    INVALID: 'bg-red-50 text-red-700 dark:bg-red-500/10 dark:text-red-300',
    UNKNOWN: 'bg-orange-50 text-orange-700 dark:bg-orange-500/10 dark:text-orange-300',
  }
  const label = status === 'UNKNOWN' ? 'UNKNOWN · UNRESOLVED' : status
  return <span className={`inline-flex whitespace-nowrap rounded-full px-2.5 py-1 text-[11px] font-semibold ${styles[status]}`}>{label}</span>
}

function ResourceStateMessage<T>({
  resource,
  label,
  onRetry,
}: {
  resource: ResourceState<T>
  label: string
  onRetry: () => void
}) {
  if (resource.data !== null) {
    return resource.error ? (
      <div className="m-4 flex flex-wrap items-center justify-between gap-3 rounded-lg border border-amber-200 bg-amber-50 p-3 text-xs text-amber-800 dark:border-amber-900 dark:bg-amber-950/30 dark:text-amber-200">
        <span>Showing the last successful {label}. Refresh failed: {resource.error}</span>
        <button className="text-button" onClick={onRetry}>Retry</button>
      </div>
    ) : null
  }
  if (resource.loading) return <div className="p-10 text-center text-sm text-slate-500">Loading {label}…</div>
  if (resource.error) {
    return <div className="m-4 rounded-lg border border-red-200 bg-red-50 p-5 text-center dark:border-red-900 dark:bg-red-950/30">
      <p className="text-sm text-red-700 dark:text-red-300">{resource.error}</p>
      <button className="secondary-button mt-3" onClick={onRetry}>Retry</button>
    </div>
  }
  return null
}

function TransactionTable({
  transactions,
  onSelect,
}: {
  transactions: TransactionDto[]
  onSelect: (transactionId: string) => void
}) {
  return <div className="overflow-x-auto"><table className="data-table">
    <thead><tr><th>Transaction</th><th>Asset</th><th>Amount</th><th>Status</th><th>Terminal</th><th>Created</th></tr></thead>
    <tbody>{transactions.map((transaction) => <tr key={transaction.transaction_id}>
      <td><button className="font-medium text-indigo-600 hover:underline dark:text-indigo-300" onClick={() => onSelect(transaction.transaction_id)}>{transaction.transaction_id}</button></td>
      <td>{transaction.asset.code}</td>
      <td className="whitespace-nowrap font-medium">{formatAmount(transaction.amount.units, transaction.amount.precision, transaction.asset.code)}</td>
      <td><StatusBadge status={transaction.status}/></td>
      <td>{transaction.terminal_id ?? '—'}</td>
      <td className="whitespace-nowrap">{formatTimestamp(transaction.created_at_unix)}</td>
    </tr>)}</tbody>
  </table></div>
}

function TransactionDetailPanel({
  transactionId,
  onClose,
}: {
  transactionId: string
  onClose: () => void
}) {
  const loader = useCallback((signal: AbortSignal) => payBridgeApi.getTransaction(transactionId, signal), [transactionId])
  const detail = useApiResource(loader, null)

  return <div className="fixed inset-0 z-[70] flex justify-end bg-slate-950/40" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose() }}>
    <section className="h-full w-full max-w-lg overflow-y-auto bg-white p-6 shadow-xl dark:bg-slate-900" role="dialog" aria-modal="true" aria-labelledby="transaction-detail-title">
      <div className="flex items-start justify-between gap-4">
        <div><p className="eyebrow">SIMULATION · TRANSACTION DETAIL</p><h2 id="transaction-detail-title" className="mt-2 break-all text-lg font-semibold">{transactionId}</h2></div>
        <button className="icon-button" aria-label="Close transaction details" onClick={onClose}><X size={16}/></button>
      </div>
      <ResourceStateMessage resource={detail} label="transaction detail" onRetry={detail.retry}/>
      {detail.data && <dl className="mt-6 grid grid-cols-2 gap-4 text-sm">
        <div><dt className="text-xs text-slate-400">Status</dt><dd className="mt-1"><StatusBadge status={detail.data.status}/></dd></div>
        <div><dt className="text-xs text-slate-400">Terminal</dt><dd className="mt-1">{detail.data.terminal_id ?? '—'}</dd></div>
        <div><dt className="text-xs text-slate-400">Amount</dt><dd className="mt-1 font-medium">{formatAmount(detail.data.amount.units, detail.data.amount.precision, detail.data.asset.code)}</dd></div>
        <div><dt className="text-xs text-slate-400">Asset type</dt><dd className="mt-1">{detail.data.asset.type}</dd></div>
        <div><dt className="text-xs text-slate-400">Created</dt><dd className="mt-1">{formatTimestamp(detail.data.created_at_unix)}</dd></div>
        <div><dt className="text-xs text-slate-400">Updated</dt><dd className="mt-1">{formatTimestamp(detail.data.updated_at_unix)}</dd></div>
      </dl>}
    </section>
  </div>
}

function ModulePlaceholder({ title }: { title: string }) {
  return <section className="panel min-h-[360px] p-6">
    <p className="eyebrow">SIMULATION · MODULE NOT CONNECTED</p>
    <h1 className="mt-2 text-xl font-semibold text-slate-950 dark:text-white">{title}</h1>
    <p className="mt-5 text-sm text-slate-500">This read-only foundation stage does not expose this module.</p>
  </section>
}

function App() {
  const [active, setActive] = useState<ModuleName>('Overview')
  const [sidebarOpen, setSidebarOpen] = useState(false)
  const [search, setSearch] = useState('')
  const [statusFilter, setStatusFilter] = useState<'ALL' | TransactionStatus>('ALL')
  const [cursorStack, setCursorStack] = useState<Array<string | null>>([null])
  const [selectedTransactionId, setSelectedTransactionId] = useState<string | null>(null)
  const cursor = cursorStack[cursorStack.length - 1] ?? null

  const transactionLoader = useCallback((signal: AbortSignal) => payBridgeApi.getTransactions({
    limit: 25,
    status: statusFilter === 'ALL' ? undefined : statusFilter,
    cursor: cursor ?? undefined,
  }, signal), [statusFilter, cursor])
  const transactionsResource = useApiResource<TransactionsResponse>(transactionLoader, 15_000)
  const terminalLoader = useCallback((signal: AbortSignal) => payBridgeApi.getTerminals(signal), [])
  const terminalsResource = useApiResource(terminalLoader, 15_000)
  const assetLoader = useCallback((signal: AbortSignal) => payBridgeApi.getAssets(signal), [])
  const assetsResource = useApiResource(assetLoader, null)
  const adapterLoader = useCallback((signal: AbortSignal) => payBridgeApi.getAdapters(signal), [])
  const adaptersResource = useApiResource(adapterLoader, null)
  const healthLoader = useCallback((signal: AbortSignal) => payBridgeApi.getHealth(signal), [])
  const healthResource = useApiResource<HealthResponse>(healthLoader, 30_000)

  useEffect(() => {
    document.documentElement.classList.add('dark')
    document.documentElement.style.colorScheme = 'dark'
  }, [])

  const mostRecentRefresh = useMemo(() => {
    const times = [transactionsResource.lastUpdated, terminalsResource.lastUpdated,
      assetsResource.lastUpdated, adaptersResource.lastUpdated,
      healthResource.lastUpdated].filter((value): value is Date => value !== null)
    return times.length ? new Date(Math.max(...times.map((value) => value.getTime()))) : null
  }, [transactionsResource.lastUpdated, terminalsResource.lastUpdated, assetsResource.lastUpdated, healthResource.lastUpdated])

  const selectModule = (name: ModuleName) => {
    setActive(name)
    setSidebarOpen(false)
  }

  const changeStatusFilter = (value: 'ALL' | TransactionStatus) => {
    setStatusFilter(value)
    setCursorStack([null])
  }

  const visibleTransactions = (transactionsResource.data?.transactions ?? []).filter((transaction) => {
    const normalized = search.trim().toLowerCase()
    if (!normalized) return true
    return [transaction.transaction_id, transaction.asset.code, transaction.terminal_id ?? '', transaction.status]
      .some((value) => value.toLowerCase().includes(normalized))
  })

  const transactionDataSection = (limit?: number) => {
    const data = limit === undefined ? visibleTransactions : visibleTransactions.slice(0, limit)
    return <>
      <ResourceStateMessage resource={transactionsResource} label="simulated transactions" onRetry={transactionsResource.retry}/>
      {transactionsResource.data && data.length > 0 && <TransactionTable transactions={data} onSelect={setSelectedTransactionId}/>}
      {transactionsResource.data && visibleTransactions.length === 0 && <div className="p-10 text-center text-sm text-slate-500">No simulated transactions match this view.</div>}
    </>
  }

  const renderModule = () => {
    if (active === 'Overview') {
      const rows = transactionsResource.data?.transactions ?? []
      const approved = rows.filter((row) => row.status === 'APPROVED').length
      const unresolved = rows.filter((row) => row.status === 'UNKNOWN').length
      const availableTerminals = terminalsResource.data?.terminals.filter((terminal) => terminal.available).length ?? 0
      return <>
        <div className="mb-6 flex flex-col justify-between gap-3 md:flex-row md:items-end">
          <div><p className="eyebrow">SIMULATED POS MONITORING</p><h1 className="mt-1 text-2xl font-semibold text-slate-950 dark:text-white">System overview</h1></div>
          <span className="text-xs text-slate-400">Latest successful refresh: {formatRefresh(mostRecentRefresh)}</span>
        </div>
        <ResourceStateMessage resource={healthResource} label="service health" onRetry={healthResource.retry}/>
        <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
          {[
            ['Transactions on this page', transactionsResource.data?.transactions.length ?? '—', Activity],
            ['Approved on this page', approved, ShieldCheck],
            ['Unresolved UNKNOWN', unresolved, LockKeyhole],
            ['Available terminals', availableTerminals, TerminalSquare],
          ].map(([title, value, Icon]) => {
            const MetricIcon = Icon as typeof LayoutDashboard
            return <article className="metric-card" key={title as string}>
              <div className="mb-4 icon-box"><MetricIcon size={17}/></div>
              <p className="text-xs font-medium text-slate-500 dark:text-slate-400">{title as string}</p>
              <p className="mt-1 text-2xl font-semibold text-slate-950 dark:text-white">{value as string | number}</p>
            </article>
          })}
        </div>
        {!healthResource.data?.persistence_ready && healthResource.data && <div className="mt-4 rounded-lg border border-amber-200 bg-amber-50 p-3 text-sm text-amber-800">SQLite persistence is not ready. Transaction reads may be unavailable.</div>}
        <section className="panel mt-5 overflow-hidden">
          <div className="panel-heading px-5 py-4"><div><p className="panel-title">Recent simulated transactions</p><p className="panel-subtitle">SQLite-backed results · current page only</p></div><button className="text-button" onClick={() => selectModule('Transactions')}>View history</button></div>
          {transactionDataSection(5)}
        </section>
      </>
    }

    if (active === 'Transactions') {
      const response = transactionsResource.data
      return <section className="panel overflow-hidden">
        <div className="flex flex-wrap items-end justify-between gap-3 px-5 py-5">
          <div><p className="eyebrow">SIMULATION · TRANSACTION HISTORY</p><h1 className="mt-1 text-xl font-semibold">Transaction monitoring</h1></div>
          <span className="text-xs text-slate-400">Last successful refresh: {formatRefresh(transactionsResource.lastUpdated)}</span>
        </div>
        <div className="flex flex-col gap-3 border-y border-slate-100 px-5 py-3 dark:border-slate-800 md:flex-row">
          <label className="search-box md:max-w-sm"><Search size={15}/><input value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search ID, asset or terminal"/><button onClick={() => setSearch('')} aria-label="Clear search" className={search ? 'visible' : 'invisible'}><X size={14}/></button></label>
          <select className="filter-select" value={statusFilter} onChange={(event) => changeStatusFilter(event.target.value as 'ALL' | TransactionStatus)}>
            <option value="ALL">All statuses</option>
            {(['CREATED', 'SENT', 'RECEIVED', 'APPROVED', 'DECLINED', 'FAILED', 'UNKNOWN', 'INVALID'] as TransactionStatus[]).map((status) => <option value={status} key={status}>{status === 'UNKNOWN' ? 'UNKNOWN · UNRESOLVED' : status}</option>)}
          </select>
          {transactionsResource.refreshing && <span className="self-center text-xs text-slate-400">Refreshing…</span>}
        </div>
        {transactionDataSection()}
        {response && <div className="flex items-center justify-between border-t border-slate-100 px-5 py-3 text-xs text-slate-500 dark:border-slate-800">
          <span>{visibleTransactions.length} result(s) on this page{response.page.has_more ? ' · more available' : ''}</span>
          <div className="flex items-center gap-2"><button className="secondary-button" disabled={cursorStack.length <= 1} onClick={() => setCursorStack((stack) => stack.slice(0, -1))}>Previous</button><span>Page {cursorStack.length}</span><button className="secondary-button" disabled={!response.page.has_more || !response.page.next_cursor} onClick={() => response.page.next_cursor && setCursorStack((stack) => [...stack, response.page.next_cursor])}>Next</button></div>
        </div>}
      </section>
    }

    if (active === 'POS Terminals') {
      const terminals = terminalsResource.data?.terminals ?? []
      return <section className="panel overflow-hidden">
        <div className="flex flex-wrap items-end justify-between gap-3 px-5 py-5"><div><p className="eyebrow">SIMULATION · READ-ONLY STATUS</p><h1 className="mt-1 text-xl font-semibold">POS terminals</h1></div><span className="text-xs text-slate-400">Last successful refresh: {formatRefresh(terminalsResource.lastUpdated)}</span></div>
        <ResourceStateMessage resource={terminalsResource} label="terminal status" onRetry={terminalsResource.retry}/>
        {terminalsResource.data && terminals.length === 0 && <p className="p-10 text-center text-sm text-slate-500">No terminals are configured.</p>}
        {terminals.length > 0 && <div className="overflow-x-auto"><table className="data-table"><thead><tr><th>Terminal</th><th>Mode</th><th>Configured</th><th>Connection</th><th>Availability</th></tr></thead><tbody>{terminals.map((terminal) => <tr key={terminal.terminal_id}><td className="font-medium">{terminal.terminal_id}</td><td>{terminal.simulated ? 'SIMULATION' : 'NOT SIMULATED'}</td><td>{terminal.configured ? 'Configured' : 'Not configured'}</td><td>{terminal.connection_status}</td><td><span className={terminal.available ? 'text-emerald-600 dark:text-emerald-300' : 'text-slate-400'}>{terminal.available ? 'Available' : 'Unavailable'}</span></td></tr>)}</tbody></table></div>}
      </section>
    }

    if (active === 'Assets') {
      const assets = assetsResource.data?.assets ?? []
      return <section className="panel overflow-hidden">
        <div className="flex flex-wrap items-end justify-between gap-3 px-5 py-5"><div><p className="eyebrow">SIMULATION · CONFIGURED ASSETS</p><h1 className="mt-1 text-xl font-semibold">Fiat and digital assets</h1></div><span className="text-xs text-slate-400">Last successful refresh: {formatRefresh(assetsResource.lastUpdated)}</span></div>
        <ResourceStateMessage resource={assetsResource} label="asset registry" onRetry={assetsResource.retry}/>
        {assetsResource.data && assets.length === 0 && <p className="p-10 text-center text-sm text-slate-500">No assets are configured.</p>}
        {assets.length > 0 && <div className="overflow-x-auto"><table className="data-table"><thead><tr><th>Asset</th><th>Type</th><th>Precision</th><th>Enabled</th></tr></thead><tbody>{assets.map((asset: AssetDto) => <tr key={`${asset.type}:${asset.code}:${asset.precision}`}><td className="font-semibold">{asset.code}</td><td>{asset.type}</td><td>{asset.precision} decimals</td><td>{asset.enabled ? 'Enabled' : 'Disabled'}</td></tr>)}</tbody></table></div>}
      </section>
    }

    if (active === 'Adapters') {
      const adapters = adaptersResource.data?.adapters ?? []
      return <section className="panel overflow-hidden">
        <div className="flex flex-wrap items-end justify-between gap-3 px-5 py-5">
          <div>
            <p className="eyebrow">READ-ONLY REGISTRY · SIMULATION ENVIRONMENT</p>
            <h1 className="mt-1 text-xl font-semibold">Terminal adapters</h1>
            <p className="mt-1 text-sm text-slate-400">Registered adapter metadata and verification boundaries.</p>
          </div>
          <span className="text-xs text-slate-400">Last successful refresh: {formatRefresh(adaptersResource.lastUpdated)}</span>
        </div>
        <ResourceStateMessage resource={adaptersResource} label="adapter registry" onRetry={adaptersResource.retry}/>
        {adaptersResource.data && adapters.length === 0 && <p className="p-10 text-center text-sm text-slate-500">No adapters are registered.</p>}
        {adapters.length > 0 && <div className="grid gap-4 p-5 md:grid-cols-2 xl:grid-cols-3">
          {adapters.map((adapter: AdapterDto) => {
            const simulated = adapter.verification_status === 'SIMULATED'
            return <article key={adapter.adapter_id} className="rounded-xl border border-slate-200 p-5 dark:border-slate-700/70">
              <div className="flex flex-wrap items-start justify-between gap-2">
                <h2 className="text-sm font-semibold text-slate-900 dark:text-white">{adapter.display_name}</h2>
                <span className={`rounded-full px-2.5 py-1 text-[10px] font-semibold ${
                  simulated
                    ? 'bg-emerald-100 text-emerald-700 dark:bg-emerald-950/60 dark:text-emerald-300'
                    : 'bg-amber-100 text-amber-800 dark:bg-amber-950/60 dark:text-amber-300'
                }`}>{adapter.verification_status}</span>
              </div>
              <p className="mt-2 text-xs text-slate-400">{adapter.manufacturer} · {adapter.device_model}</p>
              <p className="mt-4 text-[10px] font-semibold uppercase tracking-wider text-slate-400">Execution scope</p>
              <p className="mt-1 text-xs font-medium text-slate-700 dark:text-slate-200">{adapter.execution_scope}</p>
              <p className="mt-3 break-all text-[10px] text-slate-500">{adapter.adapter_id}</p>
              <p className="mt-4 text-xs leading-5 text-slate-500 dark:text-slate-400">{adapter.description}</p>
            </article>
          })}
        </div>}
      </section>
    }

    if (active === 'Logs') {
      return <section className="panel overflow-hidden"><div className="px-5 py-5"><p className="eyebrow">SIMULATION SAMPLE · NOT LIVE EVENTS</p><h1 className="mt-1 text-xl font-semibold">System logs and audit history</h1></div><div className="overflow-x-auto"><table className="data-table"><thead><tr><th>Time</th><th>Level</th><th>Event</th><th>Source</th></tr></thead><tbody>{logs.map((log, index) => <tr key={index}><td>{log.time}</td><td>{log.level}</td><td>{log.event}</td><td>{log.source}</td></tr>)}</tbody></table></div></section>
    }

    if (active === 'Settings') {
      return <section className="panel p-6"><p className="eyebrow">Appearance</p><h1 className="mt-1 text-xl font-semibold">Settings</h1><div className="mt-6 max-w-md rounded-xl border border-slate-700/70 bg-slate-900/70 p-5"><div className="flex items-center gap-3"><div className="icon-box"><Moon size={17}/></div><div><p className="text-sm font-semibold text-slate-100">Dark mode enabled</p><p className="mt-1 text-xs text-slate-400">PayBridge uses a consistent dark interface.</p></div></div></div></section>
    }

    return <ModulePlaceholder title={active}/>
  }

  return <div className="min-h-screen bg-slate-50 text-slate-700 dark:bg-[#0b1020] dark:text-slate-300">
    {sidebarOpen && <button className="fixed inset-0 z-40 bg-slate-950/40 lg:hidden" aria-label="Close navigation" onClick={() => setSidebarOpen(false)} />}
    <aside className={`sidebar ${sidebarOpen ? 'translate-x-0' : '-translate-x-full'} lg:translate-x-0`}>
      <div className="flex h-16 items-center px-5"><div className="brand-mark"><Activity size={18}/></div><div className="ml-3"><p className="text-sm font-bold text-slate-950 dark:text-white">PayBridge</p><p className="text-[10px] font-medium text-slate-400">SIMULATION DASHBOARD</p></div></div>
      <nav className="px-3 py-4"><p className="nav-label">Workspace</p>{navigation.map((item) => { const Icon = item.icon; return <button key={item.name} className={`nav-item ${active === item.name ? 'active' : ''}`} onClick={() => selectModule(item.name)}><Icon size={17}/><span>{item.name}</span></button> })}<p className="nav-label mt-7">Operations</p>{operations.map((item) => { const Icon = item.icon; return <button key={item.name} className={`nav-item ${active === item.name ? 'active' : ''}`} onClick={() => selectModule(item.name)}><Icon size={17}/><span>{item.name}</span></button> })}</nav>
      <div className="mt-auto p-3"><div className="mb-3 rounded-xl border border-slate-200 p-3 dark:border-slate-800"><p className="text-[10px] font-semibold uppercase tracking-wider text-slate-400">Backend status</p><p className="mt-1 text-xs">{healthResource.data?.persistence_ready ? 'SQLite ready' : healthResource.data ? 'Persistence unavailable' : healthResource.error ? 'Health unavailable' : 'Checking…'}</p></div><div className="nav-item"><div className="flex h-7 w-7 items-center justify-center rounded-full bg-indigo-100 text-xs font-bold text-indigo-700">PB</div><span>Local operator</span></div></div>
    </aside>
    <main className="lg:ml-[248px]">
      <header className="topbar"><button className="icon-button lg:hidden" onClick={() => setSidebarOpen(true)} aria-label="Open navigation"><ChevronRight size={18}/></button><div className="hidden items-center gap-2 text-xs text-slate-400 sm:flex"><span>PayBridge</span><span>/</span><strong className="font-medium text-slate-700 dark:text-slate-200">{active}</strong></div><div className="ml-auto flex items-center gap-3"><span className="demo-badge">SIMULATION</span><span className="hidden text-[11px] text-slate-400 md:inline">Last refresh: {formatRefresh(mostRecentRefresh)}</span><button className="avatar" aria-label="PayBridge local operator">PB</button></div></header>
      <div className="mx-auto max-w-[1440px] p-4 md:p-6">{renderModule()}</div>
    </main>
    {selectedTransactionId && <TransactionDetailPanel key={selectedTransactionId} transactionId={selectedTransactionId} onClose={() => setSelectedTransactionId(null)}/>}
  </div>
}

export default App
