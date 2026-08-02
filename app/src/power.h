/*
 * Power / battery / external-power (VBUS) handling via the nPM1300 PMIC.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_POWER_H_
#define GPS_TRACKER_POWER_H_

#include <stdbool.h>
#include <stdint.h>

/** Charge state reported in telemetry (mirrors the original 'charge' field). */
enum tracker_charge_state {
	TRACKER_CHARGE_DISCHARGING = 0, /* on battery, no external power */
	TRACKER_CHARGE_CHARGING = 1,    /* trickle / CC / CV */
	TRACKER_CHARGE_FULL = 2,        /* charge completed */
};

/**
 * @brief Initialise the PMIC/charger and register the VBUS-detect callback.
 * @return 0 on success, negative errno otherwise.
 */
int power_init(void);

/**
 * @brief Is external power (VBUS) currently present?
 *
 * Used to decide between the frequent "charging" report loop and the long
 * battery sleep, replacing the original getChargeState() plugged-in check, and
 * reported as the telemetry "vbus" field.
 *
 * Deliberately not named power_is_charging(): this is the supply, not the
 * charger. A full battery on a live USB lead is VBUS present with a charge
 * state of TRACKER_CHARGE_DISCHARGING, and the two must not be conflated.
 */
bool power_vbus_present(void);

/**
 * @brief Read just the charge state.
 *
 * Cheaper than @ref power_read when the voltage is not needed (status LED).
 * Falls back to TRACKER_CHARGE_DISCHARGING if the charger cannot be read.
 */
enum tracker_charge_state power_charge_state(void);

/**
 * @brief Read battery voltage and charge state.
 *
 * @param batt_mv       Filled with battery voltage in millivolts.
 * @param charge_state  Filled with an @ref tracker_charge_state value.
 * @return 0 on success, negative errno otherwise.
 */
int power_read(uint16_t *batt_mv, enum tracker_charge_state *charge_state);

/**
 * @brief Interruptible sleep: sleep for @p seconds, or wake early if external
 *        power is connected.
 *
 * Replaces the original PWR_SENS rising-edge wakeup.
 *
 * @return true if woken early by VBUS connect, false if the full time elapsed.
 */
bool power_wait_interruptible(uint32_t seconds);

#endif /* GPS_TRACKER_POWER_H_ */
