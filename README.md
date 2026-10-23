# ec_adapter_tcp

ESPHome external component: Modbus TCP client for **ectoControl** boiler adapters.
Mirrors the register map of the [sokolovs/ec-adapter](https://github.com/sokolovs/ec-adapter)
Home Assistant integration.

Works on any ESP32 variant with Wi-Fi (tested with ESP32-C6, ESP-IDF framework).

## Usage

## Write-only registers on first connect

`REG_W_*` registers (`0x0030`, `0x0031`..`0x0039`, `0x0080`) cannot be read:
a FC03 read returns `0` whatever was written. After a reboot the adapter
therefore keeps whatever state it had, while Home Assistant would show the
last value this device wrote.

To keep both sides in sync, the last value written by each entity is
persisted in flash (ESPHome preferences) and **pushed once on the first poll
cycle after boot**:

- `select` — the stored option (or `initial_value` if flash is empty);
- `number` — the last written value;
- `climate` — the work mode and the setpoint, but only if they were set at
  least once (an implicit default is never written, so a fresh device does not
  change the boiler state by itself).

The log shows one line per restored register:
