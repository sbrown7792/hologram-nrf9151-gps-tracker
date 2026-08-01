/*
 * Power / battery / external-power (VBUS) handling via the nPM1300 PMIC.
 *
 * VBUS connect/disconnect is detected through the nPM1300 MFD event callback
 * (pattern from nfed/samples/usb_detect). A VBUS-connect event releases a
 * semaphore so a battery sleep can be cut short and the tracker can start
 * reporting frequently, mirroring the original PWR_SENS wakeup.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "power.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

/* nPM1300 charge-status register (BCHGCHARGESTATUS) bit masks. */
#define CHG_STAT_COMPLETED BIT(1)
#define CHG_STAT_TRICKLE   BIT(2)
#define CHG_STAT_CC        BIT(3)
#define CHG_STAT_CV        BIT(4)
#define CHG_STAT_CHARGING  (CHG_STAT_TRICKLE | CHG_STAT_CC | CHG_STAT_CV)

/* VBUS status register present bit. */
#define VBUS_STAT_PRESENT BIT(0)

static const struct device *pmic = DEVICE_DT_GET(DT_NODELABEL(npm1300_pmic));
static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));

static struct gpio_callback event_cb;
static K_SEM_DEFINE(wake_sem, 0, 1);
static volatile bool vbus_connected;

static bool read_vbus_present(void)
{
	struct sensor_value val;
	int err;

	err = sensor_sample_fetch(charger);
	if (err) {
		LOG_WRN("charger sample_fetch failed (err %d)", err);
		return vbus_connected;
	}

	err = sensor_channel_get(charger,
				 (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
				 &val);
	if (err) {
		LOG_WRN("VBUS status read failed (err %d)", err);
		return vbus_connected;
	}

	return (val.val1 & VBUS_STAT_PRESENT) != 0;
}

static void event_callback(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	if (pins & BIT(NPM13XX_EVENT_VBUS_DETECTED)) {
		vbus_connected = true;
		k_sem_give(&wake_sem);
	}

	if (pins & BIT(NPM13XX_EVENT_VBUS_REMOVED)) {
		vbus_connected = false;
	}
}

int power_init(void)
{
	if (!device_is_ready(pmic) || !device_is_ready(charger)) {
		LOG_ERR("nPM1300 PMIC/charger not ready");
		return -ENODEV;
	}

	gpio_init_callback(&event_cb, event_callback,
			   BIT(NPM13XX_EVENT_VBUS_DETECTED) |
				   BIT(NPM13XX_EVENT_VBUS_REMOVED));

	int err = mfd_npm13xx_add_callback(pmic, &event_cb);

	if (err) {
		LOG_ERR("Failed to add PMIC callback (err %d)", err);
		return err;
	}

	vbus_connected = read_vbus_present();
	LOG_INF("Power init: external power %s",
		vbus_connected ? "present" : "absent");

	return 0;
}

bool power_is_charging(void)
{
	/* Refresh from hardware in case an edge was missed. */
	vbus_connected = read_vbus_present();
	return vbus_connected;
}

static enum tracker_charge_state charge_state_from_status(uint32_t status)
{
	if (status & CHG_STAT_COMPLETED) {
		return TRACKER_CHARGE_FULL;
	}

	if (status & CHG_STAT_CHARGING) {
		return TRACKER_CHARGE_CHARGING;
	}

	return TRACKER_CHARGE_DISCHARGING;
}

enum tracker_charge_state power_charge_state(void)
{
	struct sensor_value val;

	if (sensor_sample_fetch(charger) != 0) {
		return TRACKER_CHARGE_DISCHARGING;
	}

	if (sensor_channel_get(charger,
			       (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS,
			       &val) != 0) {
		return TRACKER_CHARGE_DISCHARGING;
	}

	return charge_state_from_status(val.val1);
}

int power_read(uint16_t *batt_mv, enum tracker_charge_state *charge_state)
{
	struct sensor_value val;
	int err;

	err = sensor_sample_fetch(charger);
	if (err) {
		LOG_WRN("charger sample_fetch failed (err %d)", err);
		return err;
	}

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
	if (err) {
		LOG_WRN("battery voltage read failed (err %d)", err);
		return err;
	}
	*batt_mv = (uint16_t)(val.val1 * 1000 + val.val2 / 1000);

	err = sensor_channel_get(charger,
				 (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS,
				 &val);
	if (err) {
		LOG_WRN("charge status read failed (err %d)", err);
		*charge_state = TRACKER_CHARGE_DISCHARGING;
		return 0;
	}

	*charge_state = charge_state_from_status(val.val1);

	return 0;
}

bool power_wait_interruptible(uint32_t seconds)
{
	k_sem_reset(&wake_sem);
	int r = k_sem_take(&wake_sem, K_SECONDS(seconds));

	/* r == 0  -> VBUS connect event woke us early.
	 * r == -EAGAIN -> full sleep elapsed.
	 */
	return r == 0;
}
