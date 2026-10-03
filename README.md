# ESPHome replacement for the Samsung SWL-B70F Wi-Fi kit

An ESP32-S3 that takes the place of Samsung's SWL-B70F Wi-Fi kit on older non-NASA wall units
(tested on an AR12HSFSAWKNZE, 2014): local Home Assistant control, no cloud. It also reads the
F1/F2 bus for the data the Wi-Fi kit never had.

The ESP uses two connections on the indoor unit:

| Link | What it carries | Direction |
|---|---|---|
| **F1/F2** (RS485, non-NASA, 9600 8E1) | temperatures, power, current, voltage, energy, compressor frequency, defrost, errors | read only |
| **CN51** (Wi-Fi kit connector, 5 V UART, 9600 8N1) | every command: power, mode, temperature, fan, swing, presets (Quiet, Fast, Comfort, good'sleep, Single User), auto clean, filter interval, counters | read + write |

All control goes through CN51, the way the original Wi-Fi kit did it. A command sent on F1/F2 (B0
frame) reaches the unit as a central-controller setting and cancels special modes such as Quiet,
so F1/F2 is only listened to.

## Parts

- Seeed Studio **XIAO ESP32S3 Plus**
- Seeed Studio **RS485 Breakout Board for XIAO** (TP8485E, 12 V input with on-board regulator)
- **AO3400** N-channel MOSFET (logic level)
- Resistors: **22 kΩ**, **33 kΩ**, **100 Ω**, **100 kΩ**
- Wires to the 5-pin CN51 connector and to the F1/F2 terminals

## Where things are on the indoor unit

- **F1/F2**: on the indoor unit's terminal block, next to L and N.
- **CN51**: the Wi-Fi kit connector, in the Wi-Fi module bay under the top filter.

## Wiring

### Power

The board is powered from CN51; the breakout's regulator makes the 5 V for the XIAO.

| CN51 | Breakout board |
|---|---|
| pin 5 (12 V) | 12V terminal |
| pin 4 (GND) | GND terminal (next to A/B) |

### F1/F2 → RS485 breakout

| Indoor unit | Breakout board |
|---|---|
| F1 | A |
| F2 | B |

120 Ω termination switch: **off**.

On this breakout the pins are the opposite of what the Seeed wiki suggests, seen from the ESP:

| XIAO pin | GPIO | RS485 signal |
|---|---|---|
| D5 | GPIO6 | RO (data from the bus, ESP RX) |
| D4 | GPIO5 | DI (ESP TX, unused: F1/F2 is read only) |
| D2 | GPIO3 | DE/RE |

### CN51 UART

CN51 pinout (as numbered on the indoor unit's board):

| CN51 pin | Signal |
|---|---|
| 1 | AC **RX** (input to the AC), 5 V, strong pull-up (~0.5 kΩ) inside the unit |
| 2 | AC **TX** (output from the AC), 5 V |
| 3 | reset (not connected) |
| 4 | GND |
| 5 | 12 V |

Both lines idle at 5 V.

**AC TX → ESP RX** (5 V down to 3.3 V with a divider):

```
CN51 pin 2 ──[ 22 kΩ ]──┬── XIAO D3 (GPIO4, UART RX)
                        │
                     [ 33 kΩ ]
                        │
                       GND
```

**ESP TX → AC RX** (the pull-up inside the unit is too strong for a series resistor, so a MOSFET
pulls the line low):

```
CN51 pin 1 ──[ 100 Ω ]── drain
                          AO3400
XIAO D10 (GPIO9) ─────── gate ──[ 100 kΩ ]── GND
                          source ── GND
```

The MOSFET inverts the signal, so the UART `tx_pin` is set to `inverted: true`.

### Pin summary

| XIAO pin | GPIO | Connected to |
|---|---|---|
| D2 | GPIO3 | RS485 DE/RE |
| D3 | GPIO4 | CN51 pin 2 through the 22k/33k divider |
| D4 | GPIO5 | RS485 DI |
| D5 | GPIO6 | RS485 RO |
| D10 | GPIO9 | AO3400 gate |

## Software

- [`ac-samsung.yaml`](ac-samsung.yaml): the ESPHome configuration.
- [`ac-samsung-cn51.h`](ac-samsung-cn51.h): the CN51 protocol (framing, acks, registers, auto clean
  rules), included by the YAML.
- F1/F2 uses [`samsung_ac`](https://github.com/omerfaruk-aran/esphome_samsung_hvac_bus) through
  [a fork](https://github.com/andrei-marinache/esphome_samsung_hvac_bus) that adds two hooks for
  non-NASA units: control requests go to a sink (here, CN51) instead of the bus, and the preset shown
  on the climate entity comes from CN51.

Secrets used: `api_key`, `wifi_ssid`, `wifi_password`.

## Credits

The CN51 protocol (framing, handshake, register table) comes from kumy's work on the SWL-B70F
module: [question on Reverse Engineering Stack Exchange](https://reverseengineering.stackexchange.com/q/25786)
and [samsung-ac-wifi-module](https://github.com/kumy/samsung-ac-wifi-module). Counter units follow
[samsung_ac_dplug](https://github.com/porech/samsung_ac_dplug).
