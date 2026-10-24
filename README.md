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
## Number entities

`number` writes a single 16-bit register. The ectoControl `REG_W_*` registers
are **write-only**: a read returns `0`, so the value cannot be read back from
the adapter. The component therefore keeps the last value it wrote in flash and
pushes it back once after boot.

Options:

| Option | Default | Meaning |
|---|---|---|
| `address` | — (required) | Register address, e.g. `0x0031` |
| `min_value` | `0.0` | Lower bound in user units |
| `max_value` | `100.0` | Upper bound in user units |
| `step` | `1.0` | UI step |
| `scale` | `1.0` | `raw = value * scale` (multiplier, unlike the climate's `temperature_scale` which divides) |
| `initial_value` | — | Value used when flash holds nothing yet |

What gets published, in order:

1. the last value written from this device (restored from flash), pushed to the
   adapter once the connection is up;
2. otherwise `initial_value`, also pushed to the adapter;
3. otherwise nothing — the entity stays `unknown` until you set a value, and no
   write is sent.

`initial_value` is validated against `min_value`/`max_value`. It is written to
the adapter only when the adapter acknowledges it; if the adapter is offline at
boot the hub retries, then logs a warning instead of silently losing it.
## Reference `climate` configuration

Everything except `ec_adapter_tcp_id` is optional. The defaults match the
register map of the ectoControl adapters, so a minimal entry is enough:
## Sleep (night) preset

When `sleep_temperature` is configured the climate exposes the standard
`sleep` preset and switches to it automatically:

- from `sleep_start_hour` (inclusive, default `23`) until `sleep_end_hour`
  (exclusive, default `7`) of the local time it reports
  `preset: sleep` and uses `sleep_temperature` as its setpoint;
- outside that window it reports `preset: none` and restores the normal
  setpoint, which is kept in flash and never overwritten by the night value.

The window may wrap around midnight, so the default `23 -> 7` means "23:00 to
07:00". `sleep_start_hour` and `sleep_end_hour` must differ.

Because the schedule is time driven, a `time` component is required whenever
`sleep_temperature` is set: point `time_id` at it.

> **Common pitfall:** the id must belong to a **`time:` platform**, not to a
> `sensor:`. A Home Assistant `time` *sensor* (for example a `homeassistant`
> sensor with `entity_id: sensor.time`) is a different component, and ESPHome
> rejects it with `D 'ha_time' of type homeassistant::HomeassistantSensor
> doesn't inherit from time::RealTimeClock`. Define the clock like this
> instead:
>
> ```yaml
> time:
>   - platform: homeassistant   # follows the Home Assistant time zone
>     id: ha_time
>   # or, without Home Assistant:
>   # - platform: sntp
>   #   id: ha_time
>   #   timezone: Europe/Moscow
> ```
>
> and keep a sensor with a different id if you still need one.

Manual control:

- selecting the `sleep` preset by hand switches the night setpoint on
  immediately, `none` switches it back;
- changing the target temperature by hand leaves the sleep preset, so the
  slider value is not immediately overwritten.

In both cases the automatic schedule takes over again at the next window edge.

Switching the preset only changes the setpoint this thermostat reports — the
adapter's setpoint register is **not** written, so the night value stays local.
With `auto_mode: true` the new target is still what decides whether the adapter
is engaged at the current temperature.

```yaml
time:
  - platform: homeassistant
    id: ha_time

climate:
  - platform: ec_adapter_tcp
    id: boiler
    ec_adapter_tcp_id: adapter
    name: "Boiler"
    current_temperature_sensor: room_temperature
    auto_mode: true
    sleep_temperature: 17.5
    sleep_start_hour: 23
    sleep_end_hour: 7
    time_id: ha_time