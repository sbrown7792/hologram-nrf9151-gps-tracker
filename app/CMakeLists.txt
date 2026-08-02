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

# Firmware build stamp for the telemetry "fw" field. Regenerated on every build
# rather than at configure time, so it cannot go stale; the script rewrites the
# header only when the minute has rolled over, so this does not force a rebuild
# of telemetry.c every time.
set(tracker_generated_dir ${CMAKE_CURRENT_BINARY_DIR}/include/generated)
set(tracker_build_stamp_h ${tracker_generated_dir}/tracker_build_stamp.h)

file(MAKE_DIRECTORY ${tracker_generated_dir})

add_custom_target(tracker_build_stamp ALL
	COMMAND ${CMAKE_COMMAND}
		-DOUTPUT_FILE=${tracker_build_stamp_h}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_stamp.cmake
	BYPRODUCTS ${tracker_build_stamp_h}
	COMMENT "Stamping firmware build date"
	VERBATIM
)

add_dependencies(app tracker_build_stamp)
target_include_directories(app PRIVATE ${tracker_generated_dir})
