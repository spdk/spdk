/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/env.h"
#include "spdk/vmd.h"

int g_status;

enum app_action {
	APP_ACTION_SET,
	APP_ACTION_GET,
	APP_ACTION_NOP,
};

struct app_opts {
	const char		*app_name;
	struct spdk_pci_addr	pci_addr;
	bool			all_devices;
	enum app_action		action;
	enum spdk_vmd_led_state	led_state;
};

struct app_opts g_opts = {
	.all_devices = true,
	.action = APP_ACTION_GET,
	.led_state = SPDK_VMD_LED_STATE_UNKNOWN,
};

static const char *g_led_states[] = {
	[SPDK_VMD_LED_STATE_OFF]	= "off",
	[SPDK_VMD_LED_STATE_IDENTIFY]	= "identify",
	[SPDK_VMD_LED_STATE_FAULT]	= "fault",
	[SPDK_VMD_LED_STATE_REBUILD]	= "rebuild",
	[SPDK_VMD_LED_STATE_UNKNOWN]	= "unknown",
};

static void
usage(void)
{
	printf("Usage: %s [-d] [-s STATE] [-r TRADDR]\n", g_opts.app_name);
	printf("\n");
	printf("Options:\n");
	printf("	-d		enables debug logs from the VMD module\n");
	printf("	-s STATE	sets the state of the LEDs. Available states are:\n");
	printf("			off, identify, fault, rebuild\n");
	printf("	-r TRADDR	uses device identified by TRADDR\n");
	printf("	-h		shows this help\n");
}

static int
parse_args(int argc, char **argv)
{
	int led_state;
	int op;

	g_opts.app_name = argv[0];

	while ((op = getopt(argc, argv, "dhr:s:")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_pci_addr_parse(&g_opts.pci_addr, optarg)) {
				fprintf(stderr, "Unable to parse PCI address: %s\n", optarg);
				return -EINVAL;
			}

			g_opts.all_devices = false;
			break;

		case 'd':
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
			spdk_log_set_flag("vmd");
			break;
#else
			fprintf(stderr, "%s must be rebuilt with --enable-debug for the -d flag\n",
				argv[0]);
			return -EINVAL;
#endif
		case 's':
			for (led_state = SPDK_VMD_LED_STATE_OFF;
			     led_state <= SPDK_VMD_LED_STATE_REBUILD;
			     led_state++) {
				if (strcmp(optarg, g_led_states[led_state]) == 0) {
					g_opts.led_state = (enum spdk_vmd_led_state)led_state;
					break;
				}
			}

			if (g_opts.led_state == SPDK_VMD_LED_STATE_UNKNOWN) {
				fprintf(stderr, "Invalid LED state\n");
				return -EINVAL;
			}

			g_opts.action = APP_ACTION_SET;
			break;

		case 'h':
			g_opts.action = APP_ACTION_NOP;
			usage();
			break;

		default:
			return -EINVAL;
		}
	}

	return 0;
}

static void
led_device_action(void *ctx, struct spdk_pci_device *pci_device)
{
	enum spdk_vmd_led_state led_state;
	char addr_buf[128];
	int rc;

	if (strcmp(spdk_pci_device_get_type(pci_device), "vmd") != 0) {
		return;
	}

	if (!g_opts.all_devices &&
	    spdk_pci_addr_compare(&g_opts.pci_addr, &pci_device->addr) != 0) {
		return;
	}

	rc = spdk_pci_addr_fmt(addr_buf, sizeof(addr_buf), &pci_device->addr);
	if (rc != 0) {
		fprintf(stderr, "Failed to format VMD's PCI address\n");
		g_status = 1;
		return;
	}

	if (g_opts.action == APP_ACTION_GET) {
		rc = spdk_vmd_get_led_state(pci_device, &led_state);
		if (spdk_unlikely(rc != 0)) {
			fprintf(stderr, "Failed to retrieve the state of the LED on %s\n",
				addr_buf);
			g_status = 1;
			return;
		}

		printf("%s: %s\n", addr_buf, g_led_states[led_state]);
	} else {
		rc = spdk_vmd_set_led_state(pci_device, g_opts.led_state);
		if (spdk_unlikely(rc != 0)) {
			fprintf(stderr, "Failed to set LED state on %s\n", addr_buf);
			g_status = 1;
			return;
		}
	}
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	int rc;

	if (parse_args(argc, argv) != 0) {
		usage();
		return 1;
	}

	if (g_opts.action == APP_ACTION_NOP) {
		return 0;
	}

	spdk_env_opts_init(&opts);
	opts.name = "led";

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK environment\n");
		return 1;
	}

	rc = spdk_vmd_init();
	if (rc) {
		fprintf(stderr, "Unable to initialize VMD subsystem\n");
		spdk_env_fini();
		return 1;
	}

	spdk_pci_for_each_device(NULL, led_device_action);

	spdk_vmd_fini();

	spdk_env_fini();
	return g_status;
}
