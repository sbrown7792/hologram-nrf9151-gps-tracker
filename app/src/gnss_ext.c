/*
 * External NMEA GNSS module backend (Adafruit Ultimate GPS v3 / MTK3339).
 *
 * The module streams NMEA at 9600 baud on its power-up defaults - GGA and RMC
 * at 1 Hz among others - which is exactly what Zephyr's "gnss-nmea-generic"
 * driver consumes, so there is no PMTK configuration to send and no parser to
 * write here. This file is the glue: power, lifecycle, and turning a
 * struct gnss_data into a struct tracker_fix.
 *
 * Power is an N-FET on CONFIG_TRACKER_GNSS_EXT_POWER_PIN, gate high = module
 * powered. With the CR1220 backup cell fitted the module keeps its RTC and
 * ephemerides across a power cut, so re-powering costs a warm start of a few
 * seconds rather than a ~34 s cold start - that is what makes cutting power for
 * the whole battery sleep worthwhile.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_ext.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/spinlock.h>

LOG_MODULE_REGISTER(gnss_ext, LOG_LEVEL_INF);

#define GNSS_NODE DT_NODELABEL(gnss_ext)

BUILD_ASSERT(DT_NODE_HAS_STATUS(GNSS_NODE, okay),
	     "CONFIG_TRACKER_GNSS_EXTERNAL needs a \"gnss-nmea-generic\" node labelled "
	     "gnss_ext on an enabled UART - see boards/*.overlay");

static const struct device *const gnss_dev = DEVICE_DT_GET(GNSS_NODE);
static const struct device *const uart_dev = DEVICE_DT_GET(DT_BUS(GNSS_NODE));
static const struct device *const power_port = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* Mirrors gnss_modem.c: a snapshot of the last fix plus a semaphore the waiter
 * resets, so only a report that arrives after the wait began is counted.
 */
static struct k_spinlock data_lock;
static struct tracker_fix last_fix;

static K_SEM_DEFINE(fix_sem, 0, 1);

static bool powered;

static void gnss_data_handler(const struct device *dev, const struct gnss_data *data)
{
	ARG_UNUSED(dev);

	if (data->info.fix_status == GNSS_FIX_STATUS_NO_FIX) {
		return;
	}

	/* NMEA carries no position accuracy estimate, so approximate the usual
	 * way: HDOP times the receiver's user-equivalent range error. nRF Cloud's
	 * PVT message requires the field, and the telemetry payload does not use
	 * it at all, so a rough figure is enough.
	 */
	float hdop = (float)data->info.hdop / 1000.0f;

	struct tracker_fix fix = {
		.latitude = (double)data->nav_data.latitude / 1000000000.0,
		.longitude = (double)data->nav_data.longitude / 1000000000.0,
		.altitude = (float)data->nav_data.altitude / 1000.0f,
		.accuracy = hdop * (float)CONFIG_TRACKER_GNSS_EXT_UERE_METERS,
		.speed = (float)data->nav_data.speed / 1000.0f,
		.heading = (float)data->nav_data.bearing / 1000.0f,
		.hdop = hdop,
		.satellites = data->info.satellites_cnt,
		.source = TRACKER_FIX_SOURCE_EXTERNAL,
	};

	k_spinlock_key_t key = k_spin_lock(&data_lock);

	last_fix = fix;
	k_spin_unlock(&data_lock, key);

	k_sem_give(&fix_sem);
}

/* DEVICE_DT_GET() rather than gnss_dev: the macro builds a static initialiser,
 * which a const pointer variable does not satisfy.
 */
GNSS_DATA_CALLBACK_DEFINE(DEVICE_DT_GET(GNSS_NODE), gnss_data_handler);

static void power_set(bool on)
{
	int err = gpio_pin_set(power_port, CONFIG_TRACKER_GNSS_EXT_POWER_PIN, on ? 1 : 0);

	if (err) {
		LOG_ERR("Failed to drive the GNSS power pin (err %d)", err);
		return;
	}

	powered = on;
}

int gnss_ext_init(void)
{
	if (!device_is_ready(power_port)) {
		LOG_ERR("GPIO port not ready");
		return -ENODEV;
	}

	if (!device_is_ready(uart_dev) || !device_is_ready(gnss_dev)) {
		LOG_ERR("External GNSS UART or driver not ready");
		return -ENODEV;
	}

	/* Inactive to start with: nothing should draw current until the first
	 * report cycle asks for a fix.
	 */
	int err = gpio_pin_configure(power_port, CONFIG_TRACKER_GNSS_EXT_POWER_PIN,
				     GPIO_OUTPUT_INACTIVE);

	if (err) {
		LOG_ERR("Failed to configure the GNSS power pin P0.%d (err %d)",
			CONFIG_TRACKER_GNSS_EXT_POWER_PIN, err);
		return err;
	}

	/* CONFIG_PM_DEVICE leaves the NMEA driver suspended at boot
	 * (pm_device_init_suspended), so the UART sees no traffic until
	 * gnss_ext_start() opens it. Park the UART to match.
	 */
	(void)pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);

	LOG_INF("External GNSS ready (power on P0.%d, %d baud)",
		CONFIG_TRACKER_GNSS_EXT_POWER_PIN,
		DT_PROP(DT_BUS(GNSS_NODE), current_speed));

	return 0;
}

int gnss_ext_start(void)
{
	int err;

	if (!powered) {
		power_set(true);
		if (!powered) {
			return -EIO;
		}

		/* The module needs a moment after power-up before it starts
		 * emitting sentences; opening the pipe into a dead line would
		 * only give the chat parser garbage to discard.
		 */
		k_sleep(K_MSEC(CONFIG_TRACKER_GNSS_EXT_WARMUP_MS));
	}

	err = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY) {
		LOG_ERR("Failed to resume the GNSS UART (err %d)", err);
		power_set(false);
		return err;
	}

	/* Opens the modem pipe and attaches the chat parser. The driver only
	 * implements RESUME, so this happens once and the pipe then stays
	 * attached for the lifetime of the application - suspending the UART
	 * underneath it is what actually gates the receiver.
	 */
	err = pm_device_action_run(gnss_dev, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY) {
		LOG_ERR("Failed to resume the external GNSS driver (err %d)", err);
		(void)pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
		power_set(false);
		return err;
	}

	k_sem_reset(&fix_sem);

	LOG_INF("External GNSS started");
	return 0;
}

void gnss_ext_stop(bool power_off)
{
	/* Suspend before cutting power: the UARTE pm action parks the pins in
	 * their sleep state, so TX cannot back-feed the module through its RX
	 * pin once the FET opens.
	 */
	(void)pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);

	if (power_off) {
		power_set(false);
		LOG_INF("External GNSS stopped and powered down");
	} else {
		LOG_INF("External GNSS stopped (left powered)");
	}
}

int gnss_ext_wait_fix(struct tracker_fix *out, uint32_t timeout_s)
{
	k_sem_reset(&fix_sem);

	if (k_sem_take(&fix_sem, K_SECONDS(timeout_s)) != 0) {
		LOG_WRN("External GNSS fix timed out after %u s", timeout_s);
		return -EAGAIN;
	}

	k_spinlock_key_t key = k_spin_lock(&data_lock);

	*out = last_fix;
	k_spin_unlock(&data_lock, key);

	LOG_INF("Fix (external): lat %.06f lon %.06f alt %.01f hdop %.02f acc %.01f m sats %u",
		out->latitude, out->longitude, (double)out->altitude,
		(double)out->hdop, (double)out->accuracy, out->satellites);

	return 0;
}
