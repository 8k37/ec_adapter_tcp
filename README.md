# ec_adapter_tcp

ESPHome external component: Modbus TCP client for **ectoControl** boiler adapters.
Mirrors the register map of the [sokolovs/ec-adapter](https://github.com/sokolovs/ec-adapter)
Home Assistant integration.

Works on any ESP32 variant with Wi-Fi (tested with ESP32-C6, ESP-IDF framework).

## Usage
## Register map (mirrors sokolovs/ec-adapter)

| Register | Meaning | Entity |
|---|---|---|
| `0x0030` | Connect type (0 = Adapter, 1 = Boiler Panel) | `select` |
| `0x0031` | Coolant setpoint, scale x10 | `climate` target temperature |
| `0x0039` | Work mode (0 = off, 1 = heating, 2 = DHW, 3 = heating + DHW) | `climate` mode / `select` |

Reads use FC03. Some bridges/adapters reject FC06 (write single register) and
reset the TCP connection; the client detects this once and switches all
subsequent writes to FC16 (write multiple registers). Writes are retried on a
fresh connection after `ECONNRESET` or a timeout, so a long-lived socket that
the bridge drops on write does not lose the command.