import { useEffect, useMemo, useState } from 'react'
import {
  Activity,
  Boxes,
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  CircleGauge,
  CreditCard,
  Database,
  Filter,
  LayoutDashboard,
  LockKeyhole,
  Moon,
  Search,
  Settings,
  ShieldCheck,
  Sun,
  TerminalSquare,
  WalletCards,
  X,
} from 'lucide-react'
import {
  Area,
  AreaChart,
  CartesianGrid,
  Cell,
  Pie,
  PieChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { assets, logs, statusData, transactions, volumeData } from './data/demo'
import type { ModuleName, TransactionStatus } from './data/demo'

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

function StatusBadge({ status }: { status: TransactionStatus }) {
  const styles: Record<TransactionStatus, string> = {
    RECEIVED: 'bg-indigo-50 text-indigo-700 dark:bg-indigo-500/10 dark:text-indigo-300',
    APPROVED: 'bg-emerald-50 text-emerald-700 dark:bg-emerald-500/10 dark:text-emerald-300',
    DECLINED: 'bg-amber-50 text-amber-700 dark:bg-amber-500/10 dark:text-amber-300',
    FAILED: 'bg-red-50 text-red-700 dark:bg-red-500/10 dark:text-red-300',
  }

  return <span className={`rounded-full px-2.5 py-1 text-[11px] font-semibold ${styles[status]}`}>{status}</span>
}

function ModulePlaceholder({ title }: { title: string }) {
  return (
    <section className="panel min-h-[520px] p-6">
      <div className="mb-6">
        <p className="eyebrow">Demo module</p>
        <h1 className="mt-1 text-xl font-semibold text-slate-950 dark:text-white">{title}</h1>
      </div>
      <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
        {[1, 2, 3].map((item) => (
          <div key={item} className="rounded-xl border border-slate-200 p-5 dark:border-slate-800">
            <div className="mb-4 h-10 w-10 rounded-lg bg-slate-100 dark:bg-slate-800" />
            <div className="h-3 w-28 rounded bg-slate-200 dark:bg-slate-700" />
            <div className="mt-3 h-2 w-40 rounded bg-slate-100 dark:bg-slate-800" />
          </div>
        ))}
      </div>
      <div className="mt-5 rounded-xl border border-dashed border-slate-300 p-8 text-center text-sm text-slate-500 dark:border-slate-700">
        Demo interface — no live PayBridge backend data is connected.
      </div>
    </section>
  )
}

function App() {
  const [active, setActive] = useState<ModuleName>('Overview')
  const [dark, setDark] = useState(() => localStorage.getItem('paybridge-theme') === 'dark')
  const [sidebarOpen, setSidebarOpen] = useState(false)
  const [search, setSearch] = useState('')
  const [statusFilter, setStatusFilter] = useState<'ALL' | TransactionStatus>('ALL')
  const [page, setPage] = useState(1)

  useEffect(() => {
    document.documentElement.classList.toggle('dark', dark)
    localStorage.setItem('paybridge-theme', dark ? 'dark' : 'light')
  }, [dark])

  const filteredTransactions = useMemo(() => {
    const normalized = search.toLowerCase()
    return transactions.filter((transaction) => {
      const matchesSearch = [transaction.id, transaction.reference, transaction.asset, transaction.terminal]
        .some((value) => value.toLowerCase().includes(normalized))
      const matchesStatus = statusFilter === 'ALL' || transaction.status === statusFilter
      return matchesSearch && matchesStatus
    })
  }, [search, statusFilter])

  const selectModule = (name: ModuleName) => {
    setActive(name)
    setSidebarOpen(false)
  }

  const renderModule = () => {
    if (active === 'Overview') {
      return (
        <>
          <div className="mb-6 flex flex-col justify-between gap-3 md:flex-row md:items-end">
            <div>
              <p className="eyebrow">Monday, 24 November</p>
              <h1 className="mt-1 text-2xl font-semibold text-slate-950 dark:text-white">System overview</h1>
            </div>
            <button className="secondary-button w-fit"><Filter size={15} /> Last 7 days <ChevronDown size={14} /></button>
          </div>

          <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
            {[
              ['Transaction volume', '48,294', '+12.5%', 'AED / demo'],
              ['Approved', '32,902', '+8.2%', '68.1% of volume'],
              ['Received', '8,692', '+3.1%', 'Not yet approved'],
              ['Failed / declined', '6,700', '-1.4%', 'Demo error mix'],
            ].map(([title, value, change, detail], index) => (
              <article className="metric-card" key={title}>
                <div className="mb-5 flex items-center justify-between">
                  <div className="icon-box">{index === 0 ? <CreditCard size={17} /> : index === 1 ? <ShieldCheck size={17} /> : index === 2 ? <Activity size={17} /> : <LockKeyhole size={17} />}</div>
                  <span className={index === 3 ? 'metric-neutral' : 'metric-positive'}>{change}</span>
                </div>
                <p className="text-xs font-medium text-slate-500 dark:text-slate-400">{title}</p>
                <p className="mt-1 text-2xl font-semibold text-slate-950 dark:text-white">{value}</p>
                <p className="mt-2 text-[11px] text-slate-400">{detail}</p>
              </article>
            ))}
          </div>

          <div className="mt-5 grid gap-5 xl:grid-cols-[minmax(0,1.7fr)_minmax(280px,.8fr)]">
            <section className="panel p-5">
              <div className="panel-heading">
                <div><p className="panel-title">Transaction volume</p><p className="panel-subtitle">Demo volume across supported assets</p></div>
                <button className="secondary-button">Weekly <ChevronDown size={14} /></button>
              </div>
              <div className="mt-5 h-[260px]">
                <ResponsiveContainer width="100%" height="100%">
                  <AreaChart data={volumeData}>
                    <defs><linearGradient id="volumeFill" x1="0" y1="0" x2="0" y2="1"><stop offset="5%" stopColor="#5d5fef" stopOpacity={0.22}/><stop offset="95%" stopColor="#5d5fef" stopOpacity={0}/></linearGradient></defs>
                    <CartesianGrid strokeDasharray="3 3" vertical={false} stroke="#94a3b8" opacity={0.16}/>
                    <XAxis dataKey="name" axisLine={false} tickLine={false} tick={{ fill:'#94a3b8', fontSize:11 }}/>
                    <YAxis axisLine={false} tickLine={false} tick={{ fill:'#94a3b8', fontSize:11 }}/>
                    <Tooltip contentStyle={{ borderRadius:12, border:'1px solid #e2e8f0', fontSize:12 }}/>
                    <Area type="monotone" dataKey="volume" stroke="#5d5fef" strokeWidth={2.5} fill="url(#volumeFill)" />
                  </AreaChart>
                </ResponsiveContainer>
              </div>
            </section>

            <section className="panel p-5">
              <div className="panel-heading"><div><p className="panel-title">Payment status</p><p className="panel-subtitle">Demo distribution</p></div></div>
              <div className="relative mt-3 h-[210px]">
                <ResponsiveContainer width="100%" height="100%">
                  <PieChart><Pie data={statusData} dataKey="value" innerRadius={62} outerRadius={82} paddingAngle={3}>{statusData.map((item) => <Cell key={item.name} fill={item.color}/>)}</Pie><Tooltip /></PieChart>
                </ResponsiveContainer>
                <div className="absolute inset-0 flex flex-col items-center justify-center pointer-events-none"><span className="text-2xl font-semibold">48K</span><span className="text-[11px] text-slate-400">demo total</span></div>
              </div>
              <div className="grid grid-cols-2 gap-2">{statusData.map((item) => <div key={item.name} className="flex items-center gap-2 text-xs text-slate-500"><span className="h-2 w-2 rounded-full" style={{ background:item.color }}/>{item.name}<strong className="ml-auto text-slate-800 dark:text-slate-200">{item.value}%</strong></div>)}</div>
            </section>
          </div>

          <section className="panel mt-5 overflow-hidden">
            <div className="panel-heading px-5 py-4"><div><p className="panel-title">Recent transactions</p><p className="panel-subtitle">Clearly labelled demo records</p></div><button className="text-button" onClick={() => selectModule('Transactions')}>View all</button></div>
            <div className="overflow-x-auto"><table className="data-table"><thead><tr><th>Transaction</th><th>Asset</th><th>Amount</th><th>Status</th><th>Terminal</th><th>Time</th></tr></thead><tbody>{transactions.slice(0,5).map((transaction) => <tr key={transaction.id}><td><div className="font-medium text-slate-900 dark:text-slate-100">{transaction.id}</div><div className="text-[11px] text-slate-400">{transaction.reference}</div></td><td>{transaction.asset}</td><td className="font-medium">{transaction.amount}</td><td><StatusBadge status={transaction.status}/></td><td>{transaction.terminal}</td><td>{transaction.time}</td></tr>)}</tbody></table></div>
          </section>
        </>
      )
    }

    if (active === 'Transactions') {
      return (
        <section className="panel overflow-hidden">
          <div className="px-5 py-5"><p className="eyebrow">Demo transaction ledger</p><h1 className="mt-1 text-xl font-semibold">Transaction monitoring</h1></div>
          <div className="flex flex-col gap-3 border-y border-slate-100 px-5 py-3 dark:border-slate-800 md:flex-row">
            <label className="search-box md:max-w-sm"><Search size={15}/><input value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search ID, reference or terminal"/><button onClick={() => setSearch('')} aria-label="Clear search" className={search ? 'visible' : 'invisible'}><X size={14}/></button></label>
            <select className="filter-select" value={statusFilter} onChange={(event) => setStatusFilter(event.target.value as 'ALL' | TransactionStatus)}><option value="ALL">All statuses</option><option value="RECEIVED">Received</option><option value="APPROVED">Approved</option><option value="DECLINED">Declined</option><option value="FAILED">Failed</option></select>
          </div>
          <div className="overflow-x-auto"><table className="data-table"><thead><tr><th>Transaction</th><th>Asset</th><th>Amount</th><th>Status</th><th>Terminal</th><th>Time</th></tr></thead><tbody>{filteredTransactions.map((transaction) => <tr key={transaction.id}><td><div className="font-medium text-slate-900 dark:text-slate-100">{transaction.id}</div><div className="text-[11px] text-slate-400">{transaction.reference}</div></td><td>{transaction.asset}</td><td className="font-medium">{transaction.amount}</td><td><StatusBadge status={transaction.status}/></td><td>{transaction.terminal}</td><td>{transaction.time}</td></tr>)}</tbody></table></div>
          {filteredTransactions.length === 0 && <div className="p-10 text-center text-sm text-slate-500">No demo transactions match your filters.</div>}
          <div className="flex items-center justify-between border-t border-slate-100 px-5 py-3 text-xs text-slate-500 dark:border-slate-800"><span>{filteredTransactions.length} demo records</span><div className="flex items-center gap-2"><button className="icon-button" onClick={() => setPage(Math.max(1, page - 1))}><ChevronLeft size={15}/></button><span>Page {page}</span><button className="icon-button" onClick={() => setPage(page + 1)}><ChevronRight size={15}/></button></div></div>
        </section>
      )
    }

    if (active === 'Assets') {
      return <section className="panel overflow-hidden"><div className="px-5 py-5"><p className="eyebrow">Demo registry</p><h1 className="mt-1 text-xl font-semibold">Fiat and digital assets</h1></div><div className="overflow-x-auto"><table className="data-table"><thead><tr><th>Asset</th><th>Type</th><th>Precision</th><th>Configuration</th></tr></thead><tbody>{assets.map((asset) => <tr key={asset.code}><td><div className="font-semibold">{asset.code}</div><div className="text-[11px] text-slate-400">{asset.name}</div></td><td>{asset.type}</td><td>{asset.precision} decimals</td><td><StatusBadge status={asset.status === 'Enabled' ? 'APPROVED' : 'RECEIVED'}/><span className="ml-2 text-xs text-slate-400">{asset.status}</span></td></tr>)}</tbody></table></div></section>
    }

    if (active === 'Logs') {
      return <section className="panel overflow-hidden"><div className="px-5 py-5"><p className="eyebrow">Demo events</p><h1 className="mt-1 text-xl font-semibold">System logs and audit history</h1></div><div className="overflow-x-auto"><table className="data-table"><thead><tr><th>Time</th><th>Level</th><th>Event</th><th>Source</th></tr></thead><tbody>{logs.map((log, index) => <tr key={index}><td>{log.time}</td><td><span className="rounded-full bg-slate-100 px-2 py-1 text-xs dark:bg-slate-800">{log.level}</span></td><td>{log.event}</td><td>{log.source}</td></tr>)}</tbody></table></div></section>
    }

    if (active === 'Settings') {
      return <section className="panel p-6"><p className="eyebrow">Preferences</p><h1 className="mt-1 text-xl font-semibold">Settings</h1><div className="mt-6 max-w-md rounded-xl border border-slate-200 p-5 dark:border-slate-800"><p className="text-sm font-medium">Appearance</p><p className="mt-1 text-xs text-slate-500">Choose your dashboard theme.</p><div className="mt-4 flex gap-2"><button className={!dark ? 'theme-choice active' : 'theme-choice'} onClick={() => setDark(false)}><Sun size={16}/> Light</button><button className={dark ? 'theme-choice active' : 'theme-choice'} onClick={() => setDark(true)}><Moon size={16}/> Dark</button></div></div></section>
    }

    return <ModulePlaceholder title={active} />
  }

  return (
    <div className="min-h-screen bg-slate-50 text-slate-700 dark:bg-[#0b1020] dark:text-slate-300">
      {sidebarOpen && <button className="fixed inset-0 z-40 bg-slate-950/40 lg:hidden" aria-label="Close navigation" onClick={() => setSidebarOpen(false)} />}
      <aside className={`sidebar ${sidebarOpen ? 'translate-x-0' : '-translate-x-full'} lg:translate-x-0`}>
        <div className="flex h-16 items-center px-5"><div className="brand-mark"><Activity size={18}/></div><div className="ml-3"><p className="text-sm font-bold text-slate-950 dark:text-white">PayBridge</p><p className="text-[10px] font-medium text-slate-400">ADMIN CONSOLE</p></div></div>
        <nav className="px-3 py-4">
          <p className="nav-label">Workspace</p>
          {navigation.map((item) => { const Icon = item.icon; return <button key={item.name} className={`nav-item ${active === item.name ? 'active' : ''}`} onClick={() => selectModule(item.name)}><Icon size={17}/><span>{item.name}</span></button> })}
          <p className="nav-label mt-7">Operations</p>
          {operations.map((item) => { const Icon = item.icon; return <button key={item.name} className={`nav-item ${active === item.name ? 'active' : ''}`} onClick={() => selectModule(item.name)}><Icon size={17}/><span>{item.name}</span></button> })}
        </nav>
        <div className="mt-auto p-3"><div className="mb-3 rounded-xl border border-slate-200 p-3 dark:border-slate-800"><p className="text-[10px] font-semibold uppercase tracking-wider text-slate-400">Demo environment</p><p className="mt-1 text-xs">Backend not connected</p></div><button className="nav-item"><div className="flex h-7 w-7 items-center justify-center rounded-full bg-indigo-100 text-xs font-bold text-indigo-700">PB</div><span>Demo Operator</span><ChevronDown className="ml-auto" size={14}/></button></div>
      </aside>

      <main className="lg:ml-[232px]">
        <header className="topbar"><button className="icon-button lg:hidden" onClick={() => setSidebarOpen(true)}><ChevronRight size={18}/></button><div className="hidden items-center gap-2 text-xs text-slate-400 sm:flex"><span>PayBridge</span><span>/</span><strong className="font-medium text-slate-700 dark:text-slate-200">{active}</strong></div><div className="ml-auto flex items-center gap-2"><span className="demo-badge">DEMO DATA</span><button className="icon-button" onClick={() => setDark((value) => !value)} aria-label="Toggle theme">{dark ? <Sun size={16}/> : <Moon size={16}/>}</button><button className="avatar">PB</button></div></header>
        <div className="mx-auto max-w-[1440px] p-4 md:p-6">{renderModule()}</div>
      </main>
    </div>
  )
}

export default App
