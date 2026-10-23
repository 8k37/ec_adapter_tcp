# ec_adapter_tcp

ESPHome external component: Modbus TCP client for **ectoControl** boiler adapters.
Mirrors the register map of the [sokolovs/ec-adapter](https://github.com/sokolovs/ec-adapter)
Home Assistant integration.

Works on any ESP32 variant with Wi-Fi (tested with ESP32-C6, ESP-IDF framework).

## Usage
## Write-only registers

The ectoControl adapter does not implement reads for its write registers: a
FC03 read of `0x0030`, `0x0031`, `0x0039`, `0x0032`..`0x0038` or `0x0080`
returns `0` regardless of the value that was written. The official HA
integration (`sokolovs/ec-adapter`) reflects this — only `REGISTERS_R`
(`0x0010`..`0x0023`) is polled, every `REG_W_*` entity keeps its own state.

`ec_adapter_tcp` follows the same model: `select`, `number` and `climate`
entities backed by `REG_W_*` registers are **not polled**. After the adapter
acknowledges a write the hub caches the value (`registers_[address] = value`)
and the entity publishes it; the cached value stays until the next successful
write. On boot `climate` restores its setpoint from flash, `select` and
`number` start empty until first written.

Consequence: a change made by another Modbus client (e.g. Home Assistant)
will not be reflected in these entities until it is written again from this
device. Polling them is pointless — the adapter would always answer `0`.
## Connection lifecycle

Two connection modes:

- **`persistent_connection: false` (default)** — the socket is closed after
  each successful poll cycle and reopened on the next one. One TCP handshake
  per `update_interval` (≈1 ms on a LAN), in exchange for immunity against
  bridges that drop idle connections (e.g. `modbus_bridge` with
  `send_wait_time`). Routine idle resets no longer appear in the log.
- **`persistent_connection: true`** — the socket is kept open between cycles.
  Use this only if the server never drops idle connections; otherwise every
  cycle starts with `recv failed: 104` (ECONNRESET) and a 1 s reconnect
  penalty.

In both modes a **write is never sent on a socket that has already carried
traffic**: if any request has completed on the current socket, it is closed
and reopened before the write goes out. This bypasses the bridge bug that
resets long-lived connections on the first write (FC06 or FC16 after several
reads) and was the reason writes were previously retried with a 1 s delay.

Reads that do hit a peer reset are still retried automatically; when
`persistent_connection: false`, a peer-closed read retry is logged at DEBUG
level, not WARNING.