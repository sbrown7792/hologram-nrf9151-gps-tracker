# GPS Tracker — CircuitDojo nRF9151 Feather

Port of the original Hologram Dash Arduino sketch (`../gps_tracker/gps_tracker.ino`)
to nRF Connect SDK / Zephyr.

The device acquires a GNSS fix, builds the *same* telemetry JSON the existing web app
expects, and publishes it to **nRF Cloud** over CoAP/DTLS. The payload itself is
unchanged from the Dash era, so the web app only has to unwrap one level of envelope —
see [Backend contract](#backend-contract).

Position normally comes from an [external GNSS module](#gnss-sources) rather than the
nRF9151's own receiver, which does not acquire fast enough in practice even with
assistance. The onboard receiver stays compiled in as a fallback.

The original Hologram **Cloud Socket API** transport is still selectable — the provider
is one line of `prj.conf` (see [Cloud provider](#cloud-provider)). nRF Cloud remains the
default because it is the only one that can serve
[A-GNSS assistance](#a-gnss-assistance), which matters on the fallback path.

## Behavior (mirrors the original)

- Acquire a GNSS fix from the [external module](#gnss-sources), falling back to the
  onboard receiver — with [A-GNSS assistance](#a-gnss-assistance) — if it does not lock.
  A short settle window after the first fix improves accuracy.
- Send the [telemetry payload](#telemetry-payload) — position, battery, signal and a
  little provenance.
- While externally powered (VBUS present): report every
  `TRACKER_CHARGING_INTERVAL_SECONDS` (default 60 s).
- On battery: LTE PSM + `TRACKER_SLEEP_SECONDS` (default ~9 min). Applying external
  power during the sleep wakes it early (nPM1300 VBUS-detect), replacing the original
  PWR_SENS interrupt.
- A jolt during that sleep also wakes it — see [Wake on motion](#wake-on-motion).
- Hardware watchdog resets the SoC if a cycle wedges.

## Status LED

The nPM1300 RGB LED shows connectivity at a glance:

| Colour | Meaning |
|--------|---------|
| red | no LTE registration |
| yellow | LTE registered, no GNSS fix (yet) |
| green | LTE registered **and** GNSS fix |
| dark | sleeping on battery |

The pattern carries where the power is coming from:

| Pattern | Meaning |
|---------|---------|
| solid | externally powered **and** the battery is full |
| blinking at 1 Hz | externally powered and charging |
| 100 ms flash every 2 s | running on battery |

So a continuously lit LED means one thing only — plugged in and topped up. "Full" is a
state of charge above `TRACKER_STATUS_LED_FULL_PERCENT` (95%), or the PMIC's own
charge-complete flag, whichever comes first. The threshold is the primary definition
because it does not depend on whether a charge cycle happened to run and terminate.

The on-battery pattern is a low-duty flash rather than a symmetric blink because,
unlike the charging blink, it is paid for out of the battery it is reporting on: the
sink draws ~5 mA lit, so 100 ms in 2 s averages ~0.25 mA. A short flash on a short
period is the better trade in both directions — more chances to catch it, and half
the current of a longer flash on a longer period.

Both patterns run on the system workqueue (the PMIC sinks have no hardware blink), so
they continue through reports and sleeps. The power state is re-sampled at cycle
boundaries, so a completed charge goes solid within one report interval
(`TRACKER_CHARGING_INTERVAL_SECONDS`, default 60 s).

The PMIC's LED outputs are on/off current sinks, so yellow is red + green lit
together. Taking all three channels into `host` mode
(`boards/circuitdojo_feather_nrf9151_ns.overlay`) gives up the PMIC's automatic
error/charging indication on LED0/LED1.

To save power the LED is blanked for the duration of a battery sleep, so on battery
you see the flash only while the tracker is awake. Options:

```
CONFIG_TRACKER_STATUS_LED=n                 # no LED at all
CONFIG_TRACKER_STATUS_LED_ON_BATTERY=y      # keep flashing through the battery sleep
CONFIG_TRACKER_STATUS_LED_BLINK_MS=500      # charging blink half-period -> 1 Hz
CONFIG_TRACKER_STATUS_LED_FLASH_ON_MS=100   # on-battery flash, lit
CONFIG_TRACKER_STATUS_LED_FLASH_OFF_MS=1900 # on-battery flash, dark
CONFIG_TRACKER_STATUS_LED_FULL_PERCENT=95   # at or above this, solid rather than blinking
CONFIG_TRACKER_STATUS_LED_RED_INDEX=0       # swap if the colours come out wrong
CONFIG_TRACKER_STATUS_LED_GREEN_INDEX=1
```

## GNSS sources

Two receivers are compiled in and `src/gnss.c` arbitrates between them behind a single
`gnss.h` interface, so the report loop never branches on which one is running.

**External module (preferred).** An Adafruit Ultimate GPS v3 (MTK3339) on `uart1`, whose
power-up defaults — 9600 baud, GGA + RMC at 1 Hz — are exactly what Zephyr's
`gnss-nmea-generic` driver consumes, so there is nothing to configure over the wire.
Wiring, per `boards/circuitdojo_feather_nrf9151_ns.overlay`:

| nRF9151 | Module |
|---------|--------|
| P0.24 (uart1 TX) | RX |
| P0.23 (uart1 RX) | TX |
| `TRACKER_GNSS_EXT_POWER_PIN` | N-FET gate — **high = powered** |
| GND, 3V3 | GND, VIN |

The power pin is a Kconfig value rather than a devicetree `gpios` property — the one
exception on this board — so the wiring lives with the rest of the tracker's settings:

```
CONFIG_TRACKER_GNSS_EXTERNAL=y                    # n = onboard receiver only
CONFIG_TRACKER_GNSS_EXT_POWER_PIN=13              # GPIO0 pin driving the FET gate
CONFIG_TRACKER_GNSS_EXT_WARMUP_MS=500             # boot delay before reading NMEA
CONFIG_TRACKER_GNSS_EXT_FIX_TIMEOUT_SECONDS=60    # window before falling back
CONFIG_TRACKER_GNSS_EXT_POWER_CYCLE=y             # cut power during the sleep
CONFIG_TRACKER_GNSS_EXT_MODEM_FALLBACK=y          # try the onboard receiver on failure
```

> Set `TRACKER_GNSS_EXT_POWER_PIN` to match your board before flashing. Pins 1–8, 10–12,
> 19 and 23–25 are already taken (i2c2, button, spi3, PMIC interrupt, uart0,
> accelerometer interrupts, uart1, pwm0).

Power cycling assumes the **CR1220 backup cell is fitted**: the module then keeps its
RTC and ephemerides across the cut, so the next wake is a warm start of a few seconds
instead of a ~34 s cold one. Without the cell, weigh `TRACKER_GNSS_EXT_POWER_CYCLE=n`
against the module's ~25 mA running current. The UART is suspended either way, which
both drops its idle current and parks the pins so TX cannot back-feed an unpowered
module.

`uart1` is the board's designated modem-trace UART. Tracing is not enabled here and
`uart1` is the only free serial slot on the SoC (0 is the console, 2 is i2c2, 3 is
spi3), so the overlay deletes the `nordic,modem-trace-uart` chosen and claims it.

**Onboard receiver (fallback).** `nrf_modem_gnss` in continuous 1 Hz tracking. After
`TRACKER_GNSS_EXT_FIX_TIMEOUT_SECONDS` with no fix the external module is powered down
and the rest of the cycle goes to the modem, which is slower but can be assisted. This
is also the only path on which A-GNSS does anything.

## Wake on motion

The tracker lives in a car, so a nine-minute sleep means a tow or a collision goes
unreported for up to nine minutes and then produces a single point. The onboard LIS2DH
accelerometer closes that gap: its any-motion detector raises a hardware interrupt on a
jolt, with nothing awake to poll it.

On a jolt the tracker wakes and reports every `TRACKER_CHARGING_INTERVAL_SECONDS` — the
same cadence as external power, and literally the same loop, with GNSS kept running so
each fix is instant. It stops when one of two things happens:

- **External power arrives.** The condition is `vbus || surge`, so a jolt that turns out
  to be you getting in flows straight into normal powered operation with no gap and no
  re-acquire. This is the designed false-alarm path — false alarms are cheap.
- **`TRACKER_MOTION_SURGE_SECONDS` pass with no further movement.** Measured from the
  *most recent* jolt, not the first, so an active tow keeps reporting for as long as it
  is being moved. Note that means an extended surge has no upper bound; a car on a rough
  trailer will report every 60 s until it stops or the battery does.

Reports carry `"wake": "motion"` for the whole surge, so the web app can show it as one
event rather than a run of unrelated points.

```
CONFIG_TRACKER_MOTION_WAKE=y                   # n = no accelerometer wake at all
CONFIG_TRACKER_MOTION_THRESHOLD_MG=350         # jolt threshold, ~16 mg hardware steps
CONFIG_TRACKER_MOTION_DURATION_SAMPLES=2       # debounce in samples -> 40 ms at 50 Hz
CONFIG_TRACKER_MOTION_ODR_HZ=50                # sample rate while armed
CONFIG_TRACKER_MOTION_HP_CUTOFF=2              # filter corner: 0=ODR/50 .. 3=ODR/400
CONFIG_TRACKER_MOTION_SETTLE_CYCLES=1          # sleeps to skip after losing power
CONFIG_TRACKER_MOTION_SURGE_SECONDS=600        # quiet time before returning to sleep
```

### Tuning the threshold

Three things interact, and the threshold on its own is the least useful of them.

**The threshold quantises to ~15.6 mg.** It is 7 bits of full scale, and 2 g is already
the smallest scale, so that is the hardware floor. The Kconfig value in mg rounds *down*
to the nearest step, which makes neighbouring values identical — 100, 105 and 110 mg are
all one setting. Steps near the bottom of the useful range:

| Register | Threshold |
|---|---|
| 5 | 78.1 mg |
| 6 | 93.8 mg |
| 7 | 109.4 mg |
| 8 | 125.0 mg |
| 9 | 140.6 mg |
| 10 | 156.2 mg |

**The operating mode sets the noise floor** the threshold has to clear.
`CONFIG_LIS2DH_OPER_MODE_LOW_POWER` is 8-bit and the noisiest; `NORMAL` is 10-bit and
`HIGH_RES` 12-bit. The difference between them is a few microamps — nothing next to the
modem — so if low thresholds are producing false wakes, this is the first thing to change,
not the threshold.

**The debounce rejects what is left.** `TRACKER_MOTION_DURATION_SAMPLES` requires the
threshold to be exceeded on consecutive samples, which a noise spike rarely manages and a
real jolt easily does. It is only useful if the ODR is high enough for a few samples to be
a short time: at 10 Hz the smallest debounce is 100 ms, long enough to miss a sharp impact,
which is why the default ODR is 50 Hz.

Note that the filter corner tracks the ODR (`ODR / 50` at `HP_CUTOFF=0`), so raising the
sample rate also filters out more slow movement. Raise `TRACKER_MOTION_HP_CUTOFF` to
compensate — the defaults pair 50 Hz with `ODR/200` to keep the corner near 0.25 Hz.

### Arming, and the grace period after parking

Motion is armed only on battery. While the car is running the sensor would raise a
continuous stream of events that all get discarded, each costing a transaction on the
bus it shares with the PMIC.

Losing external power means the engine just stopped, which is exactly when someone is
collecting their things and slamming doors — so arming immediately would turn every trip
into a ten-minute surge. `TRACKER_MOTION_SETTLE_CYCLES` (default 1) lets whole wake
cycles pass first:

```
power lost -> finish cycle -> sleep 9 min unarmed -> wake, report as usual
                                                  -> arm, then sleep
```

`0` arms on the first sleep instead. The count restarts whenever external power is seen
again, so every unplug gets the full grace period, but *not* on a reset: a tracker that
reboots while parked arms on its first sleep rather than leaving the car unwatched for
another cycle.

The cost is real — the car is genuinely unwatched for that first cycle after you park.
Set `0` if that matters more than the false surges do.

There is no cooldown between surges themselves: once armed, every jolt starts a new one.

### The high-pass filter is not optional

`CONFIG_LIS2DH_ACCEL_HP_FILTERS=y` and the `CTRL2` write in `motion_init()` are what make
this work at all. The LIS2DH's any-motion detector compares each axis against the
threshold in absolute terms, and the Zephyr driver never touches `CTRL2`, so the filter
defaults off. A board sitting still already reads ~1000 mg on one axis from gravity —
far above any sane threshold — so the interrupt would assert immediately and never
clear. Enabling `HPIS2` strips the DC component so only transients get through.

If the tracker wakes constantly with the board untouched, that is the first thing to
look at. The cutoff is reachable as `TRACKER_MOTION_HP_CUTOFF`; the mode is not, so
changing `HPM` means editing the `CTRL2` value in [src/motion.c](src/motion.c) —
`HPM = 11` (autoreset on interrupt, i.e. `0xC0` on top of the rest) is the next thing
to try.

The trigger mode and filter options live in a Kconfig `choice` and a driver menu, so
`TRACKER_MOTION_WAKE` cannot select them; they are set in `prj.conf` and `motion.c`
fails the build with a pointer to them if they are missing.

## A-GNSS assistance

Only ever applies to the **onboard** receiver, and only when the provider is
`"nrfcloud"` — both are runtime checks (`agnss_wanted()` in `main.c`), so
`TRACKER_AGNSS` stays selectable regardless of the provider. With the external module
doing the acquiring, this matters only after a fallback.

Without assistance a cold GNSS start hunts for satellites for a minute or more with the
receiver drawing current the whole time, so on a duty-cycled tracker it is mostly a
battery cost. nRF Cloud supplies ephemerides, almanac, time and a coarse position from
the serving cell, which normally brings a fix down to a few seconds.

It is fetched at two points, both driven by the modem rather than by guesswork:

- **Cold start.** The modem raises an assistance request within a second or two of
  starting, and only when it genuinely lacks valid data. The loop waits
  `TRACKER_AGNSS_PROACTIVE_WAIT_SECONDS` for that signal, so a warm receiver skips
  straight past at no cost.
- **Fallback.** If a fix attempt still times out after
  `TRACKER_GNSS_FIX_TIMEOUT_SECONDS`, assistance is fetched and the fix retried for
  `TRACKER_AGNSS_FIX_TIMEOUT_SECONDS`.

Downloads are rate-limited to one per `TRACKER_AGNSS_MIN_INTERVAL_SECONDS` (default
30 min), so a covered antenna cannot turn into a download every wake cycle. The
receiver is stopped during the download by default
(`TRACKER_AGNSS_STOP_GNSS_DURING_FETCH`): in LTE-M/GPS coexistence the modem
time-shares one radio, so a searching receiver competes with the transfer meant to
help it.

## Cloud provider

`prj.conf` is not tracked in git (it holds the Hologram device key). Start from the
template:

```
cp app/prj.conf.example app/prj.conf
```

One line of `prj.conf` switches providers. Nothing else needs touching — no conf
fragments, no build flags, no branch:

```
CONFIG_TRACKER_CLOUD_PROVIDER="nrfcloud"    # or "hologram"
```

Every provider-specific setting stays valid either way. That works because the nRF Cloud
library is selected unconditionally (`TRACKER_CLOUD_NRF_LIB`), so the `CONFIG_NRF_CLOUD_*`,
`CONFIG_COAP_*` and `CONFIG_DATE_TIME*` assignments in `prj.conf` always take effect —
Zephyr aborts the build on a handwritten assignment that does not, which is what used to
make the switch painful. Only `cloud_nrf.c` *or* `cloud_hologram.c` is compiled, and
`--gc-sections` drops the unreferenced library code: a `"hologram"` image comes out about
18 KB smaller than an `"nrfcloud"` one.

**nRF Cloud** needs no key in the firmware — the device authenticates with credentials in
the modem key store, installed once (see [Provisioning](#provisioning)):

```
CONFIG_TRACKER_NRF_CLOUD_APP_ID="GPSTRACKER"     # appId the backend filters on
CONFIG_TRACKER_NRF_CLOUD_PORTAL_LOCATION=y       # also plot on nRF Cloud's own map
CONFIG_TRACKER_AGNSS=n                           # disable assistance entirely
```

**Hologram** needs the 8-character device key. On the current dashboard that is under the
device's **Webhooks** tab → *Webhook key* → **Show key**, labelled **SIM Key** — not the
numeric device ID on the Device Details panel, and not the account-wide REST API key:

```
CONFIG_HOLOGRAM_DEVICE_KEY="XXXXXXXX"
```

Other options live in `Kconfig` (`TRACKER_SLEEP_SECONDS`,
`TRACKER_CHARGING_INTERVAL_SECONDS`, `TRACKER_GNSS_FIX_TIMEOUT_SECONDS`, …).

### Bench testing without a SIM

```
CONFIG_TRACKER_SKIP_LTE=y        # GNSS-only, no cellular / no SIM required
CONFIG_TRACKER_OFFLINE_DEBUG=y   # log the composed message instead of sending it
CONFIG_GNSS_DUMP_TO_LOG=y        # dump every parsed NMEA fix from the external module
```

## Provisioning

One-time, per device. The credentials live in the modem, so this is independent of the
application firmware — but `device_credentials_installer` needs the modem offline
(`AT+CFUN=4`), which the tracker firmware will not do, so provision with the `at_client`
sample flashed and then flash the tracker.

Host tooling (`pip3 install` is blocked by PEP 668 on Ubuntu 24.04; use pipx):

```bash
pipx install nrfcloud-utils         # verified against 3.3.0
# nRF Cloud portal -> Team -> API key
```

1. Flash `nrf/samples/cellular/at_client` for `circuitdojo_feather_nrf9151/nrf9151/ns`.
2. Create a local CA — once for all devices, not per device:
   ```bash
   create_ca_cert -c AU --st QLD -l Brisbane -o "Steven Brown" --cn gpstracker-ca -p certs/
   ```
3. Generate a key inside the modem, sign its CSR with that CA, and install:
   ```bash
   device_credentials_installer --ca certs/<ca>_ca.pem --ca-key certs/<ca>_prv.pem \
       --port /dev/ttyUSB0 --coap --verify --csv provision.csv
   ```
   `--coap` is essential: it installs the CoAP server root CA alongside the AWS one,
   and without it the DTLS handshake fails peer verification. The device ID defaults
   to the modem UUID, matching `CONFIG_NRF_CLOUD_CLIENT_ID_SRC_INTERNAL_UUID` — do not
   pass `--id-imei`, which would break that match and surface as a 4.01 Unauthorized at
   connect time rather than as a credential error. `--sectag` already defaults to
   16842753; add `-d` if that tag is already occupied. Do **not** pass `--local-cert`:
   by default the private key is generated inside the modem and never leaves it.
4. Register it with your nRF Cloud team:
   ```bash
   nrf_cloud_onboard --api-key <API_KEY> --csv provision.csv
   ```
5. Flash the tracker firmware. It logs its device ID at every boot — that ID is what
   the backend queries by.

Flag names drift between nrfcloud-utils releases; check `--help` if yours is not 3.3.x.

## Telemetry payload

Built by `telemetry_build_json()` and identical on both providers. New fields are
appended, never inserted, so the leading part of an old record and a new one stay
directly comparable.

```json
{"coords": [-71.149272, 41.744192], "hdop": 0.98, "batt": 90, "volt": 4062,
 "charge": 0, "signal": -92, "awake": 148, "vbus": false, "wake": "timer",
 "wdt": 0, "fw": "2026-08-02T17:51Z", "hw": "feather-nrf9151"}
```

| Field | Meaning |
|-------|---------|
| `coords` | `[longitude, latitude]`, degrees. `[0, 0]` when no fix (see `TRACKER_FORCE_PUBLISH_WITHOUT_FIX`) |
| `hdop` | Horizontal dilution of precision. `0` alongside `[0, 0]` |
| `batt` | Percent, interpolated from `volt` on a generic LiPo curve |
| `volt` | Battery millivolts. `0` means the PMIC could not be read |
| `charge` | Charger activity: `0` discharging, `1` charging, `2` complete |
| `signal` | RSRP in dBm (negative). `0` when there is no valid reading |
| `awake` | Seconds since the start of this wake cycle |
| `vbus` | External power present |
| `wake` | Why the cycle started: `boot`, `timer`, `vbus` or `motion` |
| `wdt` | Where the loop was when the watchdog last reset the device; `0` if the last boot was clean — see [Watchdog forensics](#watchdog-forensics) |
| `fw` | UTC build time of the firmware, `YYYY-MM-DDThh:mmZ` |
| `hw` | Board, from `CONFIG_TRACKER_HW_REVISION` |

Four things a reader needs to know:

- **`charge` is not the same as `vbus`.** They come from different PMIC registers:
  `charge` is what the charger is doing, `vbus` is whether a supply is attached. A
  completed charge on a live lead is `vbus: true, charge: 2`. Read "plugged in" from
  `vbus` — that is what it was added for.
- **`wake` describes the cycle, not the report.** Every report in a
  [motion surge](#wake-on-motion) is tagged `"motion"`, including the ones sent after
  external power arrives — that is what makes the surge identifiable as a single event.
  `"boot"` marks the first report after a reset, which is how a watchdog reset shows up
  in the data.
- **`charge: 0, volt: 0` means the PMIC read failed**, not a flat battery. Worth
  displaying as unknown rather than plotting a zero.
- **`charge` is not Dash-compatible.** The Konekt Dash sent its `charge_status`
  enum — `2` charging, `4` charged, `7` on battery, `0` *fault* — which collides with
  this one on every shared value. Records without an `fw` field are Dash-era and must
  be decoded with the old table. That is what `fw`/`hw` exist to disambiguate.

`fw` needs no maintenance: [`cmake/build_stamp.cmake`](cmake/build_stamp.cmake) stamps
the UTC build time into a generated header on every build, so it can never be a
version someone forgot to bump. The header is rewritten only when the minute has
rolled over, so back-to-back builds do not churn `telemetry.c`. Two consequences worth
knowing: the string sorts chronologically as plain text, and builds are not
byte-reproducible across minutes.

Set the board string with `CONFIG_TRACKER_HW_REVISION` in `prj.conf`.

Separately, [`VERSION`](VERSION) carries a semantic version that Zephyr turns into the
**MCUboot image version**. It does not appear in telemetry, but MCUboot uses it to
order images, so bump it when you cut a release you intend to deploy over DFU.

## Watchdog forensics

The watchdog resets the SoC if a cycle wedges, which is the right behaviour and also
destroys the evidence. The `wdt` field carries a breadcrumb across the reset saying where
the report loop was when it fired.

`watchdog_phase()` stores a small integer in `.noinit` at each step of the loop. That
section is not cleared by the C startup and a watchdog reset does not power-cycle the
RAM, so the value survives; `watchdog_init()` latches it, logs it, and it then rides out
in `wdt` on every subsequent report until the next reset. It is sticky rather than
one-shot so you can read it off any record, not just the one tagged `"wake": "boot"`.

| `wdt` | Phase | | `wdt` | Phase |
|---|---|---|---|---|
| 0 | clean boot, or marker not retained | | 7 | building/sending a report |
| 1 | top of the loop | | 8 | powered / surge report loop |
| 2 | `lte_ensure_connected()` | | 9 | `gnss_stop()`, incl. the UART suspend |
| 3 | `cloud_resume()` | | 10 | `cloud_pause()` |
| 4 | `gnss_start()` | | 11 | `status_led_sleep()` |
| 5 | A-GNSS fetch and inject | | 12 | arming the accelerometer (I2C) |
| 6 | waiting for a fix | | 13 | inside the battery sleep |

The numbers are sent raw and matched against `enum tracker_phase` in
[src/watchdog.h](src/watchdog.h) by hand, so only ever **append** to that enum.

**Verify retention before trusting it.** A `wdt` of 0 means either "the last boot was
clean" or "`.noinit` did not survive" — the linkage is right (the markers land above
`__bss_end`), but nothing here proves TF-M leaves non-secure RAM alone across a reset.
The boot log distinguishes the two: `Restarted from phase N (name)` versus `Cold boot, no
retained phase marker`. Force a watchdog reset on the bench once and check which you get
before reading anything into field data.

## Backend contract

Reports arrive as nRF Cloud device messages with the tracker payload as the `data`
member:

```json
{"appId":"GPSTRACKER","messageType":"DATA","ts":1754035200000,
 "data":{"coords": [151.2, -33.8], "hdop": 1.20, "batt": 87, "volt": 4010,
         "charge": 0, "signal": -85, "awake": 43, "vbus": false,
         "wake": "timer", "fw": "2026-08-02T17:51Z", "hw": "feather-nrf9151"}}
```

The web app polls nRF Cloud instead of the Hologram Data Engine and reads
`item.message.data`:

```
GET https://api.nrfcloud.com/v1/messages?deviceId=<id>&appId=GPSTRACKER&start=<ISO8601>&pageSort=desc
Authorization: Bearer <nRF Cloud API key>
```

The nRF Cloud device ID replaces the Hologram device key as the correlation key. Note
that nRF Cloud retains messages for about 30 days, so the backend must store history
itself rather than treating the cloud as the archive.

On the **Hologram** provider the record shape is different again. `/api/1/csr/rdm/`
returns each record's `data` as an escaped JSON *string*, whose own `data` member is
**base64** of the raw payload — three steps to get at the telemetry:

```
record["data"] -> JSON.parse -> ["data"] -> base64 decode -> JSON.parse
```

The `_JSONSTRING_` tag on the record is Hologram confirming the decoded bytes parse as
JSON. Sort and filter on `received`, not `logged`: `received` is when the socket server
took the message, `logged` is when the record was indexed, and indexing happens in
batches — messages that arrived a minute apart routinely share one `logged` value.

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
| `src/fix.h` | `struct tracker_fix`, the receiver-independent position |
| `src/gnss.c` | Arbitrates between the receivers; the only GNSS API main.c sees |
| `src/gnss_modem.c` | Onboard `nrf_modem_gnss` fix acquisition + assistance requests |
| `src/gnss_ext.c` | External NMEA module: power FET, UART lifecycle, fix conversion |
| `src/agnss.c` | Downloads A-GNSS assistance and injects it into the modem |
| `src/motion.c` | LIS2DH any-motion interrupt: jolt detection during the battery sleep |
| `src/wake.c` | The interruptible sleep, and which of VBUS/motion ended it |
| `src/power.c` | nPM1300 battery voltage, charge state, VBUS-detect wake |
| `src/status_led.c` | RGB status LED on the nPM1300 LED sinks |
| `src/telemetry.c` | Builds the inner JSON payload (+ LiPo battery curve) |
| `src/cloud.h` | Transport interface; one implementation is compiled in |
| `src/cloud_nrf.c` | nRF Cloud device messages over CoAP/DTLS |
| `src/cloud_hologram.c`, `src/hologram.c` | Cloud Socket envelope + TCP send (cJSON) |
| `src/watchdog.c` | Hardware watchdog safety reset + guard for blocking network I/O |
