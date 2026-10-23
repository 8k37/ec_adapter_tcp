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
## Automatic mode switch

By default the work mode is fully manual: `climate` only writes the adapter's
work-mode register when Home Assistant asks for a mode change.

With `auto_mode: true` the component acts as a thermostat and switches the
register itself:

- current temperature **below** the target temperature → `on_value`
  (adapter / heating);
- current temperature **at or above** the target temperature plus
  `auto_mode_hysteresis` (default `0.3`) → `off_value` (boiler panel / off).

Inside the dead band the current mode is kept, so the adapter is not toggled on
every poll. The switch needs a known current temperature, i.e. one of the
sources described above (an external sensor is the reliable choice: the
write-only work-mode register cannot be read back). A manual mode change from
Home Assistant is overwritten on the next update while `auto_mode` is enabled.
