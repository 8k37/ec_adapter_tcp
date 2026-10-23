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

With `auto_mode: true` the component becomes a thermostat, but **only for the
adapter, never for the mode**. The `mode` stays exactly what you commanded —
turning the climate on or off is always up to you.

While the climate is on, the thermostat decides who drives the boiler:

- current temperature **below** the target temperature → the adapter is engaged
  (`on_value`), the boiler is controlled from here;
- current temperature **at or above** the target temperature plus
  `auto_mode_hysteresis` (default `0.3`) → back to the boiler's own panel
  (`off_value`): the thermostat **idles**, the adapter does not heat.

So switching the climate on in an already warm room does not start the adapter:
it stays idle until the temperature drops below the setpoint. Inside the dead
band nothing is written, so the write-only register is not toggled on every
poll. The resulting state is published as the standard climate `action`
(`heating` / `idle` / `off`), so Home Assistant shows it without the mode
flicking back and forth.

The thermostat needs a known current temperature, i.e. one of the sources
described above (an external sensor is the reliable choice: the write-only
work-mode register cannot be read back).
