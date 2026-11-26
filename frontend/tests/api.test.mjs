import assert from 'node:assert/strict'
import { afterEach, mock, test } from 'node:test'
import { PayBridgeApiError, PayBridgeApiService } from '../src/services/api.ts'

afterEach(() => mock.restoreAll())

test('API requests use same-origin paths and never send a browser bearer token', async () => {
  let captured
  mock.method(globalThis, 'fetch', async (url, options) => {
    captured = { url: String(url), options }
    return new Response(JSON.stringify({ mode: 'SIMULATION', persistence_ready: true }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    })
  })

  const api = new PayBridgeApiService()
  const health = await api.getHealth()
  assert.deepEqual(health, { mode: 'SIMULATION', persistence_ready: true })
  assert.equal(captured.url, '/api/v1/health')
  assert.equal(captured.options.credentials, 'omit')
  assert.equal(captured.options.headers.Authorization, undefined)
  assert.equal(captured.options.headers.authorization, undefined)
})

test('transaction query is encoded and API errors are mapped to safe messages', async () => {
  let capturedUrl = ''
  mock.method(globalThis, 'fetch', async (url) => {
    capturedUrl = String(url)
    return new Response(JSON.stringify({
      error: { code: 'UNAUTHORIZED', message: 'raw backend details must not be displayed' },
    }), { status: 401, headers: { 'content-type': 'application/json' } })
  })

  const api = new PayBridgeApiService()
  await assert.rejects(
    api.getTransactions({ limit: 25, status: 'UNKNOWN', cursor: '123:PB-ABC' }),
    (error) => {
      assert.ok(error instanceof PayBridgeApiError)
      assert.equal(error.status, 401)
      assert.equal(error.code, 'UNAUTHORIZED')
      assert.match(error.message, /proxy token configuration/)
      assert.doesNotMatch(error.message, /raw backend details/)
      return true
    },
  )
  assert.equal(capturedUrl, '/api/v1/transactions?limit=25&status=UNKNOWN&cursor=123:PB-ABC')
})

test('unknown backend error bodies are not surfaced to the dashboard', async () => {
  mock.method(globalThis, 'fetch', async () => new Response(
    JSON.stringify({ error: { code: 'INTERNAL_ERROR', message: 'secret stack trace' } }),
    { status: 500, headers: { 'content-type': 'application/json' } },
  ))
  const api = new PayBridgeApiService()
  await assert.rejects(api.getAssets(), (error) => {
    assert.equal(error.message, 'Dashboard service request failed.')
    assert.doesNotMatch(error.message, /secret stack trace/)
    return true
  })
})
