import assert from 'node:assert/strict'
import { randomBytes } from 'node:crypto'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import { loadConfigFromFile } from 'vite'

test('Vite proxy injects the local token server-side and removes browser authorization', async () => {
  const previousToken = process.env.PAYBRIDGE_DASHBOARD_TOKEN
  const ephemeralToken = randomBytes(32).toString('hex')
  process.env.PAYBRIDGE_DASHBOARD_TOKEN = ephemeralToken
  try {
    const configPath = fileURLToPath(new URL('../vite.config.ts', import.meta.url))
    const loaded = await loadConfigFromFile({ command: 'serve', mode: 'test' }, configPath)
    assert.ok(loaded)
    const config = loaded.config
    assert.equal(config.server?.host, '127.0.0.1')
    assert.deepEqual(config.server?.allowedHosts, ['127.0.0.1', 'localhost'])
    assert.equal(config.preview?.host, '127.0.0.1')
    assert.deepEqual(config.preview?.allowedHosts, ['127.0.0.1', 'localhost'])
    assert.equal(config.server?.proxy?.['/api']?.target, 'http://127.0.0.1:5051')
    assert.equal(config.define, undefined)

    let proxyRequestHandler
    config.server.proxy['/api'].configure({
      on(event, handler) {
        if (event === 'proxyReq') proxyRequestHandler = handler
      },
    })
    assert.equal(typeof proxyRequestHandler, 'function')

    const requestHeaders = new Map([
      ['authorization', 'Bearer browser-controlled'],
      ['cookie', 'browser-cookie=secret'],
    ])
    const proxyRequest = {
      removeHeader: (name) => requestHeaders.delete(name.toLowerCase()),
      setHeader: (name, value) => requestHeaders.set(name.toLowerCase(), value),
    }
    proxyRequestHandler(proxyRequest, { url: '/api/v1/transactions' })
    assert.equal(requestHeaders.get('authorization'), `Bearer ${ephemeralToken}`)
    assert.equal(requestHeaders.has('cookie'), false)

    requestHeaders.set('authorization', 'Bearer browser-controlled')
    requestHeaders.set('cookie', 'browser-cookie=secret')
    proxyRequestHandler(proxyRequest, { url: '/api/v1/health' })
    assert.equal(requestHeaders.has('authorization'), false)
    assert.equal(requestHeaders.has('cookie'), false)
  } finally {
    if (previousToken === undefined) delete process.env.PAYBRIDGE_DASHBOARD_TOKEN
    else process.env.PAYBRIDGE_DASHBOARD_TOKEN = previousToken
  }
})
