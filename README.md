# PayBridge

## Backend authentication (Phase 1)

PayBridge protects payment operations with bearer-style authentication over the existing newline-delimited TCP protocol.

No authentication credential is stored in source code or frontend code.

### Development mode

Set both environment variables before starting the server and simulator:

- `PAYBRIDGE_AUTH_MODE=development`
- `PAYBRIDGE_DEV_AUTH_TOKEN=<64-character hexadecimal development bearer token>`

Generate a development token using the PayBridge test/AuthManager secure generator or an approved cryptographically secure generator. Do not commit the value.

Development mode is explicitly separate from production mode and must not be enabled with real terminal or bank credentials.

### Production mode

Set:

- `PAYBRIDGE_AUTH_MODE=production`
- `PAYBRIDGE_CREDENTIAL_TARGET=<Windows Credential Manager generic credential target>`

Store the 64-character hexadecimal bearer token in the generic credential identified by that target. The backend reads the credential through Windows Credential Manager and keeps no token in source or JSON configuration.

The server refuses to start if authentication is not configured.

### Protocol

Protected payment requests use two newline-separated records:

1. `Authorization: Bearer <64-character hexadecimal token>`
2. The existing `PAYMENT_REQUEST` JSON object with these replay-envelope fields:

   - `request_id`: exactly 32 hexadecimal characters.
   - `request_timestamp`: non-negative UTC Unix timestamp in seconds.
   - `request_expiry`: non-negative UTC Unix timestamp in seconds.
   - `external_reference`: existing mandatory compatibility field.

   `request_expiry` must be later than `request_timestamp`. Maximum request lifetime and permitted clock-skew window are both 300 seconds.

Legacy payment JSON without replay-envelope fields is rejected. Validated payment requests are fingerprinted and their request IDs are durably recorded in SQLite.

A standalone non-sensitive health request is limited to:

`HEALTH_REQUEST` JSON

It does not return transaction or configuration details.

### Credential rotation

1. Generate a new cryptographically secure 64-character hexadecimal token.
2. Store the replacement credential under the configured target or development environment.
3. Restart the backend.
4. Update authorized simulator or client configuration.
5. Remove or replace the previous credential after the rollout window.

Never print, log, or commit authorization headers or credential values.

## Remaining limitations

Authentication provides application-level bearer-token checks. It does not implement TLS, certificate provisioning, request signing, or RBAC. Durable request-ID replay protection is provided separately by the transaction persistence layer. This must not be described as production security certification.

## SQLite transaction persistence

`TransactionEngine` uses SQLite as its authoritative transaction store. The default database path is `data/paybridge.db`; set `PAYBRIDGE_DATABASE_PATH` to select another path. SQLite initialization and legacy CSV migration must complete before the server accepts requests. A database or migration error blocks startup; the engine does not fall back to writing CSV.

At startup, the engine imports `logs/transactions.csv` when that source exists. The original CSV is left untouched. Successful imports record source hashes, row counts, imported/reconciled counts and raw-row hashes in the existing migration tables. Historical status snapshots are folded into one transaction record and imported as lifecycle events. Seven-column fiat history infers precision from the registered fiat asset; current twelve-column rows require configured asset identity, including configured digital assets.

Malformed rows, unsupported assets, invalid lifecycle histories and conflicts with existing SQLite data mark migration failed and prevent startup. An unchanged completed source is reconciled and treated idempotently. Changes to a source after completed migration require manual reconciliation; they are not automatically replayed. CSV logging and parsing remain available for migration and compatibility tooling, but runtime transaction writes go only to SQLite.

The simulator stores its local attempt history separately at `data/paybridge-simulator.db` by default. Set `PAYBRIDGE_SIMULATOR_DATABASE_PATH` to choose another path. It refuses to start if its configured path equals the server path. The simulator does not import the server's CSV history.

Request IDs are persisted atomically with first-time transaction creation. Reusing an ID with the same validated request returns the recorded transaction and does not call the adapter again; reusing it with different payment details returns `REPLAY_CONFLICT`. Payment adapters and terminal transport remain simulation foundations; this change does not add real terminal processing.

## Simulator terminal workflow (Milestone 3)

Authenticated `PAYMENT_REQUEST` messages may include an optional `terminal_id`. If it is omitted, PayBridge selects a terminal only when exactly one configured simulated terminal is eligible. An explicit unknown/non-simulated terminal is rejected, and multiple eligible terminals without an explicit selection produce an ambiguity error. The built-in `CommunicationManager` simulator is `SIM-DEFAULT-001` and is configured for simulated approval.

The backend delegates payment orchestration to `PaymentOrchestrationService`. Its internal terminal exchange uses the versioned `PAYBRIDGE_SIMULATED_TERMINAL_V1` JSON simulation contract. Request and response correlation must match `request_id`, `transaction_id`, `external_reference`, and `terminal_id`. The simulated response and external payment response are marked `simulated`; messages state that no real payment was processed. This contract is internal simulation data. It is not a Verifone command set, vendor wire protocol, acquirer integration, or real payment implementation.

Successful simulated operations persist `CREATED → SENT → RECEIVED → APPROVED` or `CREATED → SENT → RECEIVED → DECLINED`. `RECEIVED` means a correlated simulated response arrived; it does not mean payment approval. Failures before sending are recorded as `FAILED`. A timeout, disconnect, malformed or mismatched response after send is recorded as `UNKNOWN`; PayBridge never automatically resends an `UNKNOWN` or in-progress operation. An identical replay returns its persisted result/state, while changed payment details under an existing request ID return `REPLAY_CONFLICT`.

The SQLite schema is version 2. Startup migrates version 1 databases in place using a transaction, adding nullable `terminal_id` and `simulation_contract` columns to `replay_records`. Existing transaction and replay rows are retained; older version 1 replay rows have null terminal metadata. Version 1 is migrated forward, while unknown or future versions are rejected. After migration, older binaries that only understand schema version 1 must not be used with that database. Expired `PROCESSING` operations are recovered at startup: `CREATED` operations become `FAILED`; `SENT` or `RECEIVED` operations become `UNKNOWN`. Non-expired in-flight rows remain `PROCESSING` and are never automatically resent.

The POS simulator targets `SIM-DEFAULT-001` and keeps a separate local attempt database. It validates the response request ID, external reference, terminal ID, and simulation marker before applying the final local status. Server-side terminal response validation additionally checks the server transaction ID.

## Vendor-independent terminal adapter boundary (Milestone 5, Stage 2)

`TerminalOperationRequest` and `TerminalOperationResult` define an exact-money, protocol-neutral operation contract. The normalized result uses `APPROVED`, `DECLINED`, `FAILED`, or `UNKNOWN` and is correlated against request ID, transaction ID, external reference, and terminal ID. `ITerminalAdapter` owns request encoding and response normalization; `ITransport` remains the byte communication boundary. `TerminalManager` registers adapters by stable adapter ID, selects only a uniquely compatible terminal/adapter, and holds a per-terminal lock from connection through the complete operation while allowing unrelated terminals to proceed independently.

`PaymentOrchestrationService` coordinates this interface and `TransactionEngine` remains authoritative for lifecycle transitions and SQLite persistence. The simulator is adapted through `SimulatedTerminalAdapter`; its JSON contract remains explicitly `PAYBRIDGE_SIMULATED_TERMINAL_V1`, and the simulated peer owns parsing/response generation outside the byte transport. No physical terminal communication is enabled. Future manufacturer/model adapters must be added as separate implementations backed by official, verified documentation and appropriate hardware; they must not add vendor commands or protocol interpretation to the transaction engine. `UNKNOWN` and unresolved `PROCESSING` results are replay-safe and are never retried automatically.
