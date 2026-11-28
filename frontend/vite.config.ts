import tailwindcss from '@tailwindcss/vite'
import react from '@vitejs/plugin-react'
import { defineConfig, type ViteDevServer } from 'vite'

function payBridgeApiOriginGuard() {
  return {
    name: 'paybridge-api-origin-guard',
    configureServer(server: ViteDevServer) {
      server.middlewares.use('/api', (request, response, next) => {
        if (request.method === 'GET') {
          return next()
        }

        const origin = request.headers.origin
        const host = request.headers.host
        const fetchSite = request.headers['sec-fetch-site']

        let sameOrigin = false
        if (origin && host) {
          try {
            sameOrigin = new URL(origin).origin === `http://${host}`
          } catch {
            sameOrigin = false
          }
        }

        if (!sameOrigin || fetchSite !== 'same-origin') {
          response.statusCode = 403
          response.setHeader('Content-Type', 'application/json')
          response.end(JSON.stringify({
            error: 'Cross-origin API request rejected',
          }))
          return
        }

        next()
      })
    },
  }
}
// https://vite.dev/config/
export default defineConfig({
  plugins: [payBridgeApiOriginGuard(), react(), tailwindcss()],
  server: {
    host: '127.0.0.1',
    allowedHosts: ['127.0.0.1', 'localhost'],
    strictPort: true,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:5051',
        changeOrigin: true,
        configure(proxy) {
          proxy.on('proxyReq', (proxyRequest, request) => {
            // The token exists only in the Vite Node process. Never forward a
            // browser-supplied Authorization header or expose it to the client.
            proxyRequest.removeHeader('authorization')
            proxyRequest.removeHeader('cookie')
            const isHealth = request.url?.split('?')[0] === '/api/v1/health'
            const token = process.env.PAYBRIDGE_DASHBOARD_TOKEN
            if (!isHealth && token) {
              proxyRequest.setHeader('Authorization', `Bearer ${token}`)
            }
          })
        },
      },
    },
  },
  preview: {
    host: '127.0.0.1',
    allowedHosts: ['127.0.0.1', 'localhost'],
    strictPort: true,
  },
})
