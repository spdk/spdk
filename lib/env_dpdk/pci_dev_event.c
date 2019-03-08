/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "env_internal.h"

static int
spdk_dev_event_callback_unregister(const char *device_name, spdk_dev_event_cb_fn cb_fn,
				   void *cb_arg)
{
	int rc;

	rc = rte_dev_event_callback_unregister(device_name, (rte_dev_event_cb_fn)cb_fn,
					       cb_arg);
	if (rc < 0) {
		SPDK_ERRLOG("Callback func is NULL\n");
	}

	return rc;
}

static int
spdk_dev_event_callback_register(const char *device_name, spdk_dev_event_cb_fn cb_fn,
				 void *cb_arg)
{
	int rc;

	rc = rte_dev_event_callback_register(device_name, (rte_dev_event_cb_fn)cb_fn,
					     cb_arg);
	if (rc) {
		SPDK_ERRLOG("Failed to register dev event callback\n");
	}

	return rc;
}

int
spdk_eal_alarm_set(uint64_t us, spdk_eal_alarm_callback cb_fn, void *cb_arg)
{
	int rc;

	rc = rte_eal_alarm_set(us, (rte_eal_alarm_callback)cb_fn,
			       cb_arg);
	if (rc) {
		SPDK_ERRLOG("Could not set up deferred callback\n");
	}

	return rc;
}

static void
spdk_dev_event_monitor_start(void)
{
	int rc;
	rc = rte_dev_event_monitor_start();
	if (rc) {
		SPDK_ERRLOG("Fail to start hotplug monitor\n");
	}
}

static void
spdk_dev_event_monitor_stop(void)
{
	int rc;

	rc = rte_dev_event_monitor_stop();
	if (rc) {
		SPDK_ERRLOG("Fail to stop hotplug monitor\n");
	}
}

void
spdk_dev_hotplug_monitor_start(const char *device_name, spdk_dev_event_cb_fn cb_fn,
			       void *cb_arg)
{
	spdk_dev_event_monitor_start();

	spdk_dev_event_callback_register(device_name, cb_fn, cb_arg);
}

void
spdk_dev_hotplug_monitor_stop(const char *device_name, spdk_dev_event_cb_fn cb_fn,
			      void *cb_arg)
{
	spdk_dev_event_callback_unregister(device_name, cb_fn, cb_arg);

	spdk_dev_event_monitor_stop();
}
