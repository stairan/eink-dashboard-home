# E-Ink Home Dashboard (v5)

ESP32 sketch that shows a Home Assistant dashboard (room temperatures and
humidity, outdoor weather, clock, battery level) on a 7.5" black-and-white
e-ink display.

## Hardware

- DFRobot FireBeetle 2 ESP32-E
- 7.5" e-ink panel (GxEPD2 driver `GxEPD2_750_GDEY075T7`)
- LiPo battery, read through an ADC pin (GPIO34)

Display wiring (set in `dashboard5.ino`): CS = 13, DC = 22, RST = 21, BUSY = 14.

## Setup

1. Install the Arduino libraries: `GxEPD2`, `ArduinoJson`. The ESP32 board
   package is also required.
2. Copy `secrets.h.example` to `secrets.h` and fill in your WiFi credentials
   and the Home Assistant host. `secrets.h` is git-ignored.
3. The sketch fetches data from `http://<HA_IP>:8100/api/v1/dashboard`.
4. Select the board **FireBeetle 2 ESP32-E** and upload.

## Flashing

Normally the Arduino IDE puts the board into flash mode automatically. If the
upload hangs on "Connecting..." (or the board is in light sleep and can't be
caught), enter flash mode manually:

1. Connect pin **D5 to GND**.
2. Press the **RESET** button. The board is now in flash mode.
3. Upload the sketch.
4. Remove the D5-GND jumper and press **RESET** again to run the sketch.

## How it works

- The ESP32 wakes on the wall-clock minute (every 60 s by day, every 5 min at
  night, 01:00-05:00) with light sleep in between, and does a fast partial
  refresh of the clock only.
- Every 10 minutes by day (30 minutes at night) it does a full refresh: reads
  the battery, connects to WiFi, syncs NTP time, fetches the sensor data,
  resets the e-ink controller and redraws the whole screen.
- A task watchdog (180 s) is armed while the board is awake and disarmed
  before sleep. If WiFi, HTTP or the display hangs, the board reboots itself.
- Timing is configured by the `DAY_TICK_SECONDS`, `NIGHT_TICK_SECONDS`,
  `DAY_FULL_REFRESH_MINUTES`, `NIGHT_FULL_REFRESH_MINUTES`, `NIGHT_*_HOUR` and
  `WATCHDOG_TIMEOUT_SECONDS` defines.

## Troubleshooting

**Data stops updating and only the clock changes; serial log shows
`Busy Timeout!`.** This comes from GxEPD2: the panel's BUSY pin never went low,
so the controller is stuck. The sketch resets the panel before every full
refresh. If a clock tick takes longer than 6 s (a BUSY timeout), it resets the
panel and forces a full redraw on the next tick (logged as
`Partial refresh took N ms - display stuck, resetting`). Pressing RESET also
clears it.

If it keeps happening, check power. The boot log prints `Reset reason: N`
(15 = brown-out, 6 = watchdog). A low battery or a voltage sag when WiFi turns
on can wedge the panel controller.
