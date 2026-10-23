# ec_adapter_tcp

ESPHome external component: Modbus TCP client for **ectoControl** boiler adapters.
Mirrors the register map of the [sokolovs/ec-adapter](https://github.com/sokolovs/ec-adapter)
Home Assistant integration.

Works on any ESP32 variant with Wi-Fi (tested with ESP32-C6, ESP-IDF framework).

## Usage

## Current temperature

`climate` shows a current temperature only if it is told where to get one.
Two options:

- **`current_temperature_address`** (recommended) — a *readable* register of
  the adapter that holds the boiler's measured temperature, e.g. the coolant
  sensor. Add `current_temperature_scale` if the raw value is scaled
  (default `1.0`). The value is read as a signed 16-bit integer, so sub-zero
  readings work.
- **`sources`** — external sensors (this component's `sensor`, a Home
  Assistant sensor, …). When at least one source is configured it takes
  precedence over `current_temperature_address`.
