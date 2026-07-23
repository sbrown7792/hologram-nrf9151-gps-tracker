/*
 * GNSS antenna configuration hook.
 *
 * On the nRF9151 Feather the auxiliary GNSS antenna is selected with the modem
 * AT command %XANTCFG. This runs automatically right after the modem library is
 * initialised. Lifted from nfed/samples/gps/src/startup.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>

LOG_MODULE_REGISTER(startup, LOG_LEVEL_INF);

#if defined(CONFIG_BOARD_CIRCUITDOJO_FEATHER_NRF9151)
#define AUXANTCFG_ENABLE "AT\%XANTCFG=1"

NRF_MODEM_LIB_ON_INIT(aux_init_hook, on_modem_lib_init, NULL);

static void on_modem_lib_init(int ret, void *ctx)
{
	ARG_UNUSED(ctx);

	if (ret != 0) {
		return;
	}

	LOG_INF("Setting GNSS antenna configuration: %s", AUXANTCFG_ENABLE);
	int err = nrf_modem_at_printf("%s", AUXANTCFG_ENABLE);

	if (err) {
		LOG_ERR("Failed to set antenna configuration (err: %d)", err);
	}
}
#endif /* CONFIG_BOARD_CIRCUITDOJO_FEATHER_NRF9151 */
