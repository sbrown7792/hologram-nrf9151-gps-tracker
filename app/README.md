# GPS Tracker — CircuitDojo nRF9151 Feather

Port of the original Hologram Dash Arduino sketch (`../gps_tracker/gps_tracker.ino`)
to nRF Connect SDK / Zephyr.

The device acquires a GNSS fix from the nRF9151's onboard GNSS, builds the *same*
telemetry JSON the existing web app expects, and publishes it to **nRF Cloud** over
CoAP/DTLS. The payload itself is unchanged from the Dash era, so the web app only has
to unwrap one level of envelope — see [Backend contract](#backend-contract).

The original Hologram **Cloud Socket API** transport is still selectable
(`CONFIG_TRACKER_CLOUD_HOLOGRAM`), but it needs an 8-character Data Engine device key,
which Hologram does not issue for Hyper-provisioned SIMs.

## Behavior (mirrors the original)

- Acquire a GNSS fix (waits up to `TRACKER_GNSS_FIX_TIMEOUT_SECONDS`, then a short
  settle window for better accuracy), with
  [A-GNSS assistance](#a-gnss-assistance) when the modem needs it.
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

## A-GNSS assistance

Without assistance a cold GNSS start hunts for satellites for a minute or more with the
receiver drawing current the whole time, so on a duty-cycled tracker it is mostly a
battery cost. nRF Cloud supplies ephemerides, almanac, time and a coarse position from
the serving cell, which normally brings a fix down to a few seconds.

It is fetched at two points, both driven by the modem rather than by guesswork:

- **Cold start.** The modem raises an assistance request within a second or two of
  `gnss_start()`, and only when it genuinely lacks valid data. The loop waits
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

## Configuration

nRF Cloud needs no key in the firmware — the device authenticates with credentials in
the modem key store, installed once (see [Provisioning](#provisioning)). Useful knobs:

```
CONFIG_TRACKER_NRF_CLOUD_APP_ID="GPSTRACKER"     # appId the backend filters on
CONFIG_TRACKER_NRF_CLOUD_PORTAL_LOCATION=y       # also plot on nRF Cloud's own map
CONFIG_TRACKER_AGNSS=n                           # disable assistance entirely
```

For the Hologram transport instead, set `CONFIG_TRACKER_CLOUD_HOLOGRAM=y` and the
8-character device key from the dashboard (device → *Receive from Device* — not the
numeric device ID):

```
CONFIG_HOLOGRAM_DEVICE_KEY="XXXXXXXX"
```

Other options live in `Kconfig` (`TRACKER_SLEEP_SECONDS`,
`TRACKER_CHARGING_INTERVAL_SECONDS`, `TRACKER_GNSS_FIX_TIMEOUT_SECONDS`, …).

### Bench testing without a SIM

```
CONFIG_TRACKER_SKIP_LTE=y        # GNSS-only, no cellular / no SIM required
CONFIG_TRACKER_OFFLINE_DEBUG=y   # log the composed message instead of sending it
```

## Provisioning

One-time, per device. The credentials live in the modem, so this is independent of the
application firmware — but `device_credentials_installer` needs the modem offline
(`AT+CFUN=4`), which the tracker firmware will not do, so provision with the `at_client`
sample flashed and then flash the tracker.

```bash
pip3 install nrfcloud-utils          # host tooling
# nRF Cloud portal -> Team -> API key
```

1. Flash `nrf/samples/cellular/at_client` for `circuitdojo_feather_nrf9151/nrf9151/ns`.
2. Create a local CA — once for all devices, not per device:
   ```bash
   create_ca_cert -c AU -st QLD -l Brisbane -o "Steven Brown" -cn gpstracker-ca -p certs/
   ```
3. Generate a key inside the modem, sign it with that CA, and install:
   ```bash
   device_credentials_installer -d --ca certs/<ca>_ca.pem --ca-key certs/<ca>_prv.pem \
       --port /dev/ttyUSB0 --sectag 16842753 --id-uuid --coap --csv provision.csv
   ```
   `--coap` is essential: it installs the CoAP root CA alongside the AWS one, and
   without it the DTLS handshake fails peer verification. `--id-uuid` makes the
   certificate CN the modem UUID, which must match
   `CONFIG_NRF_CLOUD_CLIENT_ID_SRC_INTERNAL_UUID` — a mismatch shows up as a 4.01
   Unauthorized at connect time, not as a credential error. Flag spellings drift
   between nrfcloud-utils releases; check `--help`.
4. Register it with your nRF Cloud team:
   ```bash
   nrf_cloud_onboard --api-key <API_KEY> --csv provision.csv
   ```
5. Flash the tracker firmware. It logs its device ID at every boot — that ID is what
   the backend queries by.

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
| `src/gnss.c` | `nrf_modem_gnss` fix acquisition + assistance requests |
| `src/agnss.c` | Downloads A-GNSS assistance and injects it into the modem |
| `src/power.c` | nPM1300 battery voltage, charge state, VBUS-detect wake |
| `src/status_led.c` | RGB status LED on the nPM1300 LED sinks |
| `src/telemetry.c` | Builds the inner JSON payload (+ LiPo battery curve) |
| `src/cloud.h` | Transport interface; one implementation is compiled in |
| `src/cloud_nrf.c` | nRF Cloud device messages over CoAP/DTLS |
| `src/cloud_hologram.c`, `src/hologram.c` | Cloud Socket envelope + TCP send (cJSON) |
| `src/watchdog.c` | Hardware watchdog safety reset + guard for blocking network I/O |
