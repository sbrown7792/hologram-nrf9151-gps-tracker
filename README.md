# GPS Tracker — CircuitDojo nRF9151 Feather

A GNSS + telemetry tracker for the [CircuitDojo nRF9151 Feather](https://www.jaredwolff.com/store/nrf9160-feather/),
built on the nRF Connect SDK / Zephyr. It acquires a GNSS fix — from an external
NMEA module where one is fitted, otherwise from the nRF9151's own receiver —
builds a telemetry JSON payload, and publishes it to **nRF Cloud** over CoAP/DTLS
or to the **Hologram Data Engine** over the Embedded **Cloud Socket API**,
whichever `CONFIG_TRACKER_CLOUD_PROVIDER` selects. See
[`app/README.md`](app/README.md) for the application's behavior, configuration,
and source layout.

This is a **freestanding (Zephyr "T2") application**. It does not vendor the SDK.
The [`west.yml`](west.yml) manifest imports CircuitDojo's
[nRF9160 Feather Examples and Drivers](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers)
(nfed), which provides the `circuitdojo_feather_nrf9151` board definition and in
turn imports the nRF Connect SDK and the `pcf85063a` RTC driver.

## Getting started

You need the [nRF Connect SDK toolchain](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)
(`west`, the Zephyr SDK, etc.) installed. Then bootstrap a workspace from this repo:

```sh
west init -m https://github.com/sbrown7792/hologram-nrf9151-gps-tracker gps-tracker-ws
cd gps-tracker-ws
west update
```

This clones the SDK and nfed alongside this repo (which lands in `application/`), about
3 GB across nine repositories.

### Keeping the SDK out of a synced folder

If this repo lives somewhere that gets synchronised — Nextcloud, Dropbox, iCloud — you do
not want a hundred thousand SDK files in the sync queue. West insists everything sits
under one workspace root, so the way out is to put the workspace on local disk and reach
the app through a symlink:

```sh
mkdir -p ~/dev/gps-tracker-ws && cd ~/dev/gps-tracker-ws
ln -s /path/to/synced/application application

# `west init -l` calls resolve() on the manifest path, follows the symlink, and
# would create the workspace back in the synced folder. Write the config by hand:
mkdir .west
printf '[manifest]\npath = application\nfile = west.yml\n\n[zephyr]\nbase = zephyr\n' > .west/config

west update
```

`west topdir` should now report `~/dev/gps-tracker-ws`. Your code stays synced; every
dependency lands outside the synced tree.

**Always run `west` from the workspace root.** It locates the workspace by walking up
from the *physical* working directory, so from inside the symlinked `application/` it
would walk up into the synced folder and not find `.west` at all.

## Build

```sh
# From the workspace root, with the toolchain environment activated:
west build -b circuitdojo_feather_nrf9151/nrf9151/ns -d ~/dev/build/gps application/app
```

Output: `merged.hex` in the build directory (MCUboot + TF-M + app). Flash via the
CircuitDojo serial bootloader (`newtmgr`/`mcumgr`, MODE button) or a J-Link / probe-rs.
Keep the build directory out of the synced tree too — it is another ~1 GB.

Before a real build, set the GPIO pin driving the external GNSS module's power FET
and — for the Hologram provider — the device key. See
[`app/README.md`](app/README.md#gnss-sources) and
[Cloud provider](app/README.md#cloud-provider).

## License

The application sources under [`app/`](app/) are licensed under Apache-2.0
(`SPDX-License-Identifier: Apache-2.0`). The nRF Connect SDK and nfed are
fetched by `west` and retain their own licenses.
