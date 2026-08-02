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

## Backend contract

Reports arrive as nRF Cloud device messages with the tracker payload as the `data`
member:

```json
{"appId":"GPSTRACKER","messageType":"DATA","ts":1754035200000,
 "data":{"coords": [151.2, -33.8], "hdop": 1.20, "batt": 87, "volt": 4010,
         "charge": 0, "signal": -85, "awake": 43}}
```

The web app polls nRF Cloud instead of the Hologram Data Engine and reads
`item.message.data`, which is byte-for-byte what the Dash used to send:

```
GET https://api.nrfcloud.com/v1/messages?deviceId=<id>&appId=GPSTRACKER&start=<ISO8601>&pageSort=desc
Authorization: Bearer <nRF Cloud API key>
```

The nRF Cloud device ID replaces the Hologram device key as the correlation key. Note
that nRF Cloud retains messages for about 30 days, so the backend must store history
itself rather than treating the cloud as the archive.

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
| `src/power.c` | nPM1300 battery voltage, charge state, VBUS-detect wake |
| `src/status_led.c` | RGB status LED on the nPM1300 LED sinks |
| `src/telemetry.c` | Builds the inner JSON payload (+ LiPo battery curve) |
| `src/cloud.h` | Transport interface; one implementation is compiled in |
| `src/cloud_nrf.c` | nRF Cloud device messages over CoAP/DTLS |
| `src/cloud_hologram.c`, `src/hologram.c` | Cloud Socket envelope + TCP send (cJSON) |
| `src/watchdog.c` | Hardware watchdog safety reset + guard for blocking network I/O |
