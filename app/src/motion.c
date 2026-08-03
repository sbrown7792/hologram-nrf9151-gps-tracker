/*
 * Wake-on-motion via the onboard LIS2DH accelerometer. See motion.h.
 *
 * The sensor's "any motion" (AOI) detector compares each axis against a
 * threshold and drives INT2 - P0.12 on this board - which the Zephyr driver
 * surfaces as SENSOR_TRIG_DELTA. P0.12 is inside the board's sense-edge-mask,
 * so it wakes the SoC through the GPIO SENSE path rather than tying up a GPIOTE
 * channel for the whole sleep.
 *
 * The high-pass filter is the part that makes this work at all. With it off -
 * which is the reset default, and the Zephyr driver never touches CTRL2 - the
 * detector compares *absolute* acceleration, so the ~1 g of gravity a stationary
 * board already reads would sit permanently above any sane threshold and hold
 * the interrupt asserted forever. HPIS2 removes the DC component so only
 * transients get through.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "motion.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wake.h"

LOG_MODULE_REGISTER(motion, LOG_LEVEL_INF);

BUILD_ASSERT(IS_ENABLED(CONFIG_LIS2DH_TRIGGER),
	     "CONFIG_TRACKER_MOTION_WAKE needs the LIS2DH trigger support that carries "
	     "sensor_trigger_set(): add CONFIG_LIS2DH_TRIGGER_GLOBAL_THREAD=y to prj.conf");

/* LIS2DH CTRL2 (0x21), from the datasheet - the driver's register definitions
 * are private to zephyr/drivers/sensor/st/lis2dh/lis2dh.h, and the only route to
 * this register is SENSOR_ATTR_CONFIGURATION, which writes the byte verbatim.
 *
 *   bits 7:6 HPM   high-pass mode, 00 = normal
 *   bits 5:4 HPCF  cutoff select, 00 = highest (ODR/50)
 *   bit  3   FDS   filter the *output* data too - left off, we want raw samples
 *   bit  1   HPIS2 apply the filter to the AOI2 (any-motion) generator
 */
#define LIS2DH_CTRL2_HPIS2 BIT(1)
#define LIS2DH_CTRL2_VALUE LIS2DH_CTRL2_HPIS2

static const struct device *const accel = DEVICE_DT_GET(DT_ALIAS(accel0));

/* The driver keeps the pointer (lis2dh->trig_anymotion = trig), so this cannot
 * be a stack local.
 */
static const struct sensor_trigger motion_trig = {
	.type = SENSOR_TRIG_DELTA,
	.chan = SENSOR_CHAN_ACCEL_XYZ,
};

static bool usable;
static bool armed;

/* Written from the trigger handler, read by the report loop. */
static atomic_t last_event_ms_hi;
static atomic_t last_event_ms_lo;

static void motion_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trig);

	int64_t now = k_uptime_get();

	/* Two halves rather than a lock: this runs on the system workqueue and
	 * the reader is the main thread, so a 64-bit store is not atomic on
	 * Cortex-M. Written high-then-low, read low-then-high, and a torn read
	 * can only ever make the surge look marginally shorter.
	 */
	atomic_set(&last_event_ms_hi, (atomic_val_t)(now >> 32));
	atomic_set(&last_event_ms_lo, (atomic_val_t)(now & 0xffffffff));

	LOG_INF("Jolt detected");

	wake_signal(TRACKER_WAKE_MOTION);
}

static int set_attr(const char *what, enum sensor_attribute attr,
		    const struct sensor_value *val)
{
	int err = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, attr, val);

	if (err) {
		LOG_ERR("Failed to set %s (err %d)", what, err);
	}

	return err;
}

int motion_init(void)
{
	if (!device_is_ready(accel)) {
		LOG_ERR("Accelerometer not ready; motion wake unavailable");
		return -ENODEV;
	}

	/* Full scale first: the driver interprets the slope threshold against
	 * whatever range is current when it is written. 2 g gives the finest
	 * threshold steps (~15.6 mg) and costs nothing - the threshold register
	 * is 7 bits of full scale either way, and we only care that a jolt
	 * crosses the line, not how hard it was.
	 *
	 * Mind the units: SENSOR_ATTR_FULL_SCALE is m/s^2, which the driver puts
	 * back through sensor_ms2_to_g(). Passing a bare 2 asks for 2 m/s^2,
	 * which rounds to 0 g and is rejected with -EINVAL.
	 */
	struct sensor_value val;

	sensor_g_to_ms2(2, &val);

	if (set_attr("full scale", SENSOR_ATTR_FULL_SCALE, &val)) {
		return -EIO;
	}

	val = (struct sensor_value){ .val1 = CONFIG_TRACKER_MOTION_ODR_HZ, .val2 = 0 };
	if (set_attr("sampling frequency", SENSOR_ATTR_SAMPLING_FREQUENCY, &val)) {
		return -EIO;
	}

	/* See the CTRL2 comment above - without this the detector never clears. */
	val = (struct sensor_value){ .val1 = LIS2DH_CTRL2_VALUE, .val2 = 0 };
	if (set_attr("high-pass filter", SENSOR_ATTR_CONFIGURATION, &val)) {
		return -EIO;
	}

	int64_t ums2 = (int64_t)CONFIG_TRACKER_MOTION_THRESHOLD_MG * 980665 / 100;

	val = (struct sensor_value){ .val1 = (int32_t)(ums2 / 1000000),
				     .val2 = (int32_t)(ums2 % 1000000) };
	if (set_attr("slope threshold", SENSOR_ATTR_SLOPE_TH, &val)) {
		return -EIO;
	}

	/* Duration is in samples, so the wall-clock time it represents follows
	 * the ODR above: N / TRACKER_MOTION_ODR_HZ seconds of sustained motion.
	 */
	val = (struct sensor_value){ .val1 = CONFIG_TRACKER_MOTION_DURATION_SAMPLES,
				     .val2 = 0 };
	if (set_attr("slope duration", SENSOR_ATTR_SLOPE_DUR, &val)) {
		return -EIO;
	}

	usable = true;

	LOG_INF("Motion wake ready (%d mg, %d samples at %d Hz, surge %d s)",
		CONFIG_TRACKER_MOTION_THRESHOLD_MG, CONFIG_TRACKER_MOTION_DURATION_SAMPLES,
		CONFIG_TRACKER_MOTION_ODR_HZ, CONFIG_TRACKER_MOTION_SURGE_SECONDS);

	return 0;
}

void motion_set_armed(bool arm)
{
	if (!usable || arm == armed) {
		return;
	}

	int err = sensor_trigger_set(accel, &motion_trig, arm ? motion_handler : NULL);

	if (err) {
		LOG_ERR("Failed to %s the jolt interrupt (err %d)",
			arm ? "arm" : "disarm", err);
		return;
	}

	armed = arm;
	LOG_DBG("Jolt interrupt %s", arm ? "armed" : "disarmed");
}

int64_t motion_last_event_uptime(void)
{
	uint32_t lo = (uint32_t)atomic_get(&last_event_ms_lo);
	uint32_t hi = (uint32_t)atomic_get(&last_event_ms_hi);

	return ((int64_t)hi << 32) | lo;
}
