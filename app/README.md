# GPS Tracker — CircuitDojo nRF9151 Feather

Port of the original Hologram Dash Arduino sketch (`../gps_tracker/gps_tracker.ino`)
to nRF Connect SDK / Zephyr.

The device acquires a GNSS fix from the nRF9151's onboard GNSS, builds the *same*
telemetry JSON the existing web app expects, and pushes it into the **Hologram Data
Engine** over Hologram's Embedded **Cloud Socket API** (a plain TCP connection to
`cloudsocket.hologram.io:9999`). Because the data still lands in the Hologram Data
Engine, the existing REST API and web-app frontend work unchanged.

## Behavior (mirrors the original)

- Acquire a GNSS fix (waits up to `TRACKER_GNSS_FIX_TIMEOUT_SECONDS`, then a short
  settle window for better accuracy).
- Send `{"coords":[lon,lat],"hdop":H,"batt":%,"volt":mV,"charge":C,"signal":S,"awake":s}`.
- While externally powered (VBUS present): report every
  `TRACKER_CHARGING_INTERVAL_SECONDS` (default 60 s).
- On battery: LTE PSM + `k_sleep(TRACKER_SLEEP_SECONDS)` (default ~9 min). Applying
  external power during the sleep wakes it early (nPM1300 VBUS-detect), replacing the
  original PWR_SENS interrupt.
- Hardware watchdog resets the SoC if a cycle wedges.

## Status LED

The nPM1300 RGB LED shows connectivity at a glance:

| Colour | Meaning |
|--------|---------|
| red | no LTE registration |
| yellow | LTE registered, no GNSS fix (yet) |
| green | LTE registered **and** GNSS fix |
| dark | sleeping on battery |

The pattern carries the charge state on top of that colour:

| Pattern | Meaning |
|---------|---------|
| blinking | battery charging (trickle / CC / CV) |
| solid | charge complete, or running on battery |

Blinking runs on the system workqueue (the PMIC sinks have no hardware blink), so
it continues through reports and sleeps. The charge state itself is re-sampled at
cycle boundaries, so a completed charge goes solid within one report interval
(`TRACKER_CHARGING_INTERVAL_SECONDS`, default 60 s).

The PMIC's LED outputs are on/off current sinks, so yellow is red + green lit
together. Taking all three channels into `host` mode
(`boards/circuitdojo_feather_nrf9151_ns.overlay`) gives up the PMIC's automatic
error/charging indication on LED0/LED1.

To save power the LED is blanked for the duration of a battery sleep and only lit
while the tracker is awake; while externally powered it stays lit. Options:

```
CONFIG_TRACKER_STATUS_LED=n              # no LED at all
CONFIG_TRACKER_STATUS_LED_ON_BATTERY=y   # keep it lit through the battery sleep
CONFIG_TRACKER_STATUS_LED_BLINK_MS=500   # charging blink half-period
CONFIG_TRACKER_STATUS_LED_RED_INDEX=0    # swap if the colours come out wrong
CONFIG_TRACKER_STATUS_LED_GREEN_INDEX=1
```

## Configuration

`prj.conf` is not tracked in git (it holds the device key). Start from the
template:

```
cp app/prj.conf.example app/prj.conf
```

Then set at least the Hologram device key (dashboard → device →
*Receive from Device*):

```
CONFIG_HOLOGRAM_DEVICE_KEY="XXXXXXXX"
```

Other options live in `Kconfig` (`TRACKER_SLEEP_SECONDS`,
`TRACKER_CHARGING_INTERVAL_SECONDS`, `TRACKER_GNSS_FIX_TIMEOUT_SECONDS`, …).

### Bench testing without a SIM

```
CONFIG_TRACKER_SKIP_LTE=y        # GNSS-only, no cellular / no SIM required
CONFIG_TRACKER_OFFLINE_DEBUG=y   # log the composed Hologram envelope instead of sending
```

## Build

Bootstrap the workspace first (see the [top-level README](../README.md)), then
from the **workspace root** with your toolchain environment activated:

```
source ~/.zephyrtools/env/bin/activate
export ZEPHYR_BASE=$PWD/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/.zephyrtools/toolchain/zephyr-sdk-0.16.4
export PATH="$HOME/.zephyrtools/ninja:$PATH"

west build -b circuitdojo_feather_nrf9151/nrf9151/ns application/app
```

Output: `build/merged.hex` (MCUboot + TF-M + app). Flash via the CircuitDojo serial
bootloader (`newtmgr`/`mcumgr`, MODE button) or a J-Link/probe-rs.

## Source layout

| File | Responsibility |
|------|----------------|
| `src/main.c` | Orchestration: connect → fix → report → charge-loop / sleep |
| `src/startup.c` | `AT%XANTCFG=1` GNSS antenna hook (nRF9151) |
| `src/gnss.c` | `nrf_modem_gnss` fix acquisition |
| `src/power.c` | nPM1300 battery voltage, charge state, VBUS-detect wake |
| `src/status_led.c` | RGB status LED on the nPM1300 LED sinks |
| `src/telemetry.c` | Builds the inner JSON payload (+ LiPo battery curve) |
| `src/hologram.c` | Cloud Socket envelope + TCP send (cJSON) |
| `src/watchdog.c` | Hardware watchdog safety reset |
