# Microcontroller-based Voltage Protector (FSMB Recruitment 2026, Phase 1)

Simulated low-voltage voltage/over-current protector on an Arduino Uno (ATmega328P), built and run in Wokwi.
**Nothing is connected to mains.** Grid conditions are emulated with potentiometers; the "appliance" is an LED behind a relay.

## What it does
| Feature | Behaviour |
|---|---|
| Over-voltage | Relay opens after the trip delay (default 200 ms); a surge above OV + 30 V opens it on the next 10 ms scan |
| Under-voltage | Relay opens after the trip delay |
| Over-current | Inverse-time trip (about 1 s at 2x the limit, slower for small overloads, faster for big ones); short circuit (3x limit, max 19 A) trips immediately; **latched** until RESET |
| Reconnect | Voltage faults auto-reconnect once voltage is inside the window (5 V hysteresis) for the reconnect delay; same delay at power-up |
| Local settings | 16x2 LCD + 3 buttons: SEL = next page, UP/DN = change value; UP on the status page = RESET |
| Computer settings | Serial 9600 8N1: `HELP`, `STATUS`, `GET`, `SET OV|UV|OC|TDLY|RDLY <v>`, `RESET`, `DEFAULTS`, `LOG ON|OFF` |
| Persistence | Settings stored in EEPROM, validated on boot, bad values fall back to defaults |

Defaults: OV 250 V, UV 180 V, OC 5.0 A, trip delay 200 ms, reconnect delay 5 s.
Scaling: RV1 (A0) 0-5 V = 0-300 V, RV2 (A1) 0-5 V = 0-20 A.

## Run it in Wokwi (about 2 minutes)
1. Go to https://wokwi.com, **New Project**, choose **Arduino Uno**.
2. Replace the contents of `sketch.ino` with `src/sketch.ino`.
3. Open the `diagram.json` tab and replace its contents with `project_files/diagram.json`.
4. Open `libraries.txt` (create it if missing) and paste `LiquidCrystal`.
5. Press **Start simulation**. The serial monitor is at the bottom; type commands and press Enter.
or,simply go to https://wokwi.com/projects/477406594723296257
If the relay clicks the wrong way (load ON while the system says OFF), set `RELAY_ACTIVE_HIGH` to `0` at the top of the sketch.

### Pin map
| Signal | Pin | Signal | Pin |
|---|---|---|---|
| Voltage pot RV1 | A0 | Relay IN | D7 |
| Current pot RV2 | A1 | Fault LED (red) | D6 |
| SEL / UP / DN buttons | D8 / D9 / D10 | OK LED (green) | D13 |
| LCD RS, E, D4-D7 | D12, D11, D5, D4, D3, D2 | Buzzer | A2 |

## Quick test script
1. Start with RV1 near 230 V and RV2 near 2 A. The load turns ON after about 5 s.
2. Turn RV1 up to about 265 V: the relay opens after about 200 ms. Return to 230 V: it reconnects after 5 s.
3. Jump RV1 to about 295 V: it opens immediately.
4. Turn RV1 down to about 150 V: under-voltage trip, then auto-reconnect.
5. Turn RV2 to about 7 A: it trips after a few seconds and latches (red LED solid, buzzer). Lower RV2 and send `RESET` or press UP on the status page.
6. Send `SET OV 270`, `SET OC 7.5`, `GET`; then change a value from the buttons (SEL to a page, UP/DN) and confirm with `GET`.



## Repository layout
```
src/sketch.ino              firmware
design_document.pdf         one-page report
schematics/                 real-product block diagram + Wokwi wiring
test_evidence/              screenshots and serial logs
project_files/              diagram.json, libraries.txt, sketch.ino copy for Wokwi
readme.md
```
