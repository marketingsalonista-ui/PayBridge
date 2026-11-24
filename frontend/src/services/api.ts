// Separate API boundary for future PayBridge integration.
// This dashboard currently uses demo data and makes no backend requests.

export interface ApiRequestOptions {
  signal?: AbortSignal
}

export class PayBridgeApiService {
  private readonly baseUrl: string

  constructor(baseUrl = '/api') {
    this.baseUrl = baseUrl
  }

  async getHealth(_options: ApiRequestOptions = {}) {
    throw new Error(
      `PayBridge API integration is not configured. Intended endpoint: ${this.baseUrl}/health`
    )
  }

  async getTransactions(_options: ApiRequestOptions = {}) {
    throw new Error(
      `PayBridge API integration is not configured. Intended endpoint: ${this.baseUrl}/transactions`
    )
  }

  async getAssets(_options: ApiRequestOptions = {}) {
    throw new Error(
      `PayBridge API integration is not configured. Intended endpoint: ${this.baseUrl}/assets`
    )
  }
}

export const payBridgeApi = new PayBridgeApiService()
