#
# GPS Tracker for CircuitDojo nRF9151 Feather
#
# Port of the original Hologram Dash Arduino sketch (../gps_tracker/gps_tracker.ino)
# to nRF Connect SDK / Zephyr. Sends GPS + telemetry as a JSON string to nRF Cloud
# (or the Hologram Data Engine), preserving the existing web-app payload schema.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gps_tracker)

target_sources(app PRIVATE
	src/main.c
	src/startup.c
	src/gnss.c
	src/gnss_modem.c
	src/power.c
	src/telemetry.c
	src/watchdog.c
)

# Exactly one transport is compiled in, so main.c never needs a #if.
target_sources_ifdef(CONFIG_TRACKER_CLOUD_NRF app PRIVATE src/cloud_nrf.c)
target_sources_ifdef(CONFIG_TRACKER_CLOUD_HOLOGRAM app PRIVATE
	src/cloud_hologram.c
	src/hologram.c
)

# The external GNSS module is additive: gnss.c arbitrates between it and the
# onboard receiver, which is always built.
target_sources_ifdef(CONFIG_TRACKER_GNSS_EXTERNAL app PRIVATE src/gnss_ext.c)

target_sources_ifdef(CONFIG_TRACKER_AGNSS app PRIVATE src/agnss.c)
target_sources_ifdef(CONFIG_TRACKER_STATUS_LED app PRIVATE src/status_led.c)
