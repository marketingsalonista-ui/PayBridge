export type TransactionStatus =
  | 'CREATED'
  | 'SENT'
  | 'RECEIVED'
  | 'APPROVED'
  | 'DECLINED'
  | 'FAILED'
  | 'UNKNOWN'
  | 'INVALID'

export interface ApiErrorDto {
  error: {
    code: string
    message: string
  }
}

export interface HealthResponse {
  mode: 'SIMULATION'
  persistence_ready: boolean
}

export interface TransactionDto {
  transaction_id: string
  status: TransactionStatus
  created_at_unix: number
  updated_at_unix: number
  amount: {
    units: string
    precision: number
  }
  asset: {
    code: string
    type: 'FIAT' | 'DIGITAL_ASSET'
    precision: number
  }
  terminal_id?: string
}

export interface TransactionsResponse {
  transactions: TransactionDto[]
  page: {
    limit: number
    has_more: boolean
    next_cursor: string | null
  }
}

export interface TransactionTerminalDto {
  terminal_id: string
  simulated: boolean
  configured: boolean
  connection_status: 'DISCONNECTED' | 'CONNECTING' | 'CONNECTED' | 'UNKNOWN' | 'FAILED'
  available: boolean
}

export interface TerminalsResponse {
  terminals: TransactionTerminalDto[]
}

export interface AssetDto {
  code: string
  type: 'FIAT' | 'DIGITAL_ASSET'
  precision: number
  enabled: boolean
}

export interface AssetsResponse {
  assets: AssetDto[]
}

export interface TransactionQuery {
  limit?: number
  status?: TransactionStatus
  cursor?: string
}

const SAFE_ERROR_MESSAGES: Record<string, string> = {
  UNAUTHORIZED: 'Dashboard authentication failed. Check the local proxy token configuration.',
  AUTH_UNAVAILABLE: 'Dashboard authentication is unavailable.',
  INVALID_QUERY: 'The transaction filters or page cursor are invalid.',
  INVALID_PATH: 'The requested path is invalid.',
  INVALID_TRANSACTION_ID: 'The transaction ID is invalid.',
  NOT_FOUND: 'The requested transaction was not found.',
  PERSISTENCE_UNAVAILABLE: 'Transaction storage is unavailable.',
  METHOD_NOT_ALLOWED: 'This dashboard endpoint only supports GET.',
  BODY_NOT_ALLOWED: 'This dashboard endpoint does not accept a request body.',
}

export class PayBridgeApiError extends Error {
  readonly status: number
  readonly code: string

  constructor(status: number, code: string, message: string) {
    super(message)
    this.name = 'PayBridgeApiError'
    this.status = status
    this.code = code
  }
}

function safeError(responseStatus: number, payload: unknown): PayBridgeApiError {
  const errorDto = payload && typeof payload === 'object' && 'error' in payload
    ? (payload as Partial<ApiErrorDto>).error
    : undefined
  const code = typeof errorDto?.code === 'string' ? errorDto.code : 'REQUEST_FAILED'
  const message = SAFE_ERROR_MESSAGES[code] ??
    (responseStatus === 401
      ? 'Dashboard authentication failed.'
      : 'Dashboard service request failed.')
  return new PayBridgeApiError(responseStatus, code, message)
}

export class PayBridgeApiService {
  private readonly baseUrl: string

  constructor(baseUrl = '/api/v1') {
    this.baseUrl = baseUrl
  }

  private async get<T>(path: string, signal?: AbortSignal): Promise<T> {
    let response: Response
    try {
      response = await fetch(`${this.baseUrl}${path}`, {
        method: 'GET',
        headers: { Accept: 'application/json' },
        cache: 'no-store',
        credentials: 'omit',
        signal,
      })
    } catch (error) {
      if (signal?.aborted) throw error
      throw new PayBridgeApiError(0, 'NETWORK_ERROR', 'Dashboard service could not be reached.')
    }

    let payload: unknown
    try {
      payload = await response.json()
    } catch {
      payload = undefined
    }
    if (!response.ok) throw safeError(response.status, payload)
    if (payload === null || typeof payload !== 'object') {
      throw new PayBridgeApiError(502, 'INVALID_RESPONSE', 'Dashboard returned an invalid response.')
    }
    return payload as T
  }

  getHealth(signal?: AbortSignal): Promise<HealthResponse> {
    return this.get('/health', signal)
  }

  getTransactions(query: TransactionQuery = {}, signal?: AbortSignal): Promise<TransactionsResponse> {
    const params = new URLSearchParams()
    if (query.limit !== undefined) params.set('limit', String(query.limit))
    if (query.status) params.set('status', query.status)
    if (query.cursor) params.set('cursor', query.cursor)
    // The current API cursor contract uses one literal colon separator; its
    // timestamp and transaction ID are validated before the URI is parsed.
    const encoded = params.toString().replaceAll('%3A', ':')
    const search = params.size > 0 ? `?${encoded}` : ''
    return this.get(`/transactions${search}`, signal)
  }

  getTransaction(transactionId: string, signal?: AbortSignal): Promise<TransactionDto> {
    return this.get(`/transactions/${encodeURIComponent(transactionId)}`, signal)
  }

  getTerminals(signal?: AbortSignal): Promise<TerminalsResponse> {
    return this.get('/terminals', signal)
  }

  getAssets(signal?: AbortSignal): Promise<AssetsResponse> {
    return this.get('/assets', signal)
  }
}

export const payBridgeApi = new PayBridgeApiService()
