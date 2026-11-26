# PayBridge Simulation Dashboard

The React dashboard reads transactions, terminals, assets, and service health
from the authenticated PayBridge API. All transaction and terminal data is
simulation-only. `UNKNOWN` transactions are displayed as unresolved.

## Local development

Run the backend on `127.0.0.1:5051` and configure the same 64-character
hexadecimal development bearer token in the backend and in the Vite process.
In PowerShell, set the non-`VITE_` variable before starting Vite:

```powershell
$env:PAYBRIDGE_AUTH_MODE = 'development'
$env:PAYBRIDGE_DEV_AUTH_TOKEN = '<64-character-hex-token>'
$env:PAYBRIDGE_DASHBOARD_TOKEN = $env:PAYBRIDGE_DEV_AUTH_TOKEN
npm run dev
```

Vite binds to `127.0.0.1:5173`, permits only `127.0.0.1` and `localhost`, and
proxies `/api` to `http://127.0.0.1:5051`. The token is read from the Vite Node
process and attached to proxied protected requests. It is not compiled into
frontend assets, read by browser JavaScript, stored in browser storage, or
returned in API responses. Health requests are forwarded without a token.

The production build is static and does not include a backend proxy. A deployed
dashboard must use a trusted same-origin server-side proxy that injects
credentials and protects the API. Do not call the loopback API directly from a
browser or add the bearer token to a `VITE_` variable.

## Checks

```powershell
npm test
npm run build
```
