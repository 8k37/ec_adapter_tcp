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