# ec_adapter_tcp

ESPHome external component: Modbus TCP client for **ectoControl** boiler adapters.
Mirrors the register map of the [sokolovs/ec-adapter](https://github.com/sokolovs/ec-adapter)
Home Assistant integration.

Works on any ESP32 variant with Wi-Fi (tested with ESP32-C6, ESP-IDF framework).

## Usage

## Current temperature

`climate` shows a current temperature only if it is told where to get one.
Three options, in order of precedence:

- **`current_temperature_sensor`** (recommended when the temperature comes from
  Home Assistant) — the `id` of any ESPHome sensor, including a
  `homeassistant` platform sensor. It becomes the active temperature source.
- **`sources`** — several named external sensors; the active one is picked by
  the stored selection.
- **`current_temperature_address`** — a *readable* register of the adapter
  itself (e.g. the coolant sensor). Add `current_temperature_scale` if the raw
  value is scaled (default `1.0`); it is read as a signed 16-bit value, so
  sub-zero readings work.

A configured source (either form) always takes precedence over
`current_temperature_address`.
