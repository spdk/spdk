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
#include "spdk/vmd.h"

struct spdk_pci_addr g_probe_addr;

static int
parse_args(int argc, char **argv)
{
	int op;

	while ((op = getopt(argc, argv, "r:")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_pci_addr_parse(&g_probe_addr, optarg)) {
				SPDK_ERRLOG("Error parsing PCI address\n");
				return 1;
			}
			break;

		default:
			return 1;
		}
	}

	return 0;
}

static void *
hp_thread(void *vmd)
{
	bool is_inserted, hp_event;
	struct spdk_pci_addr addr = {-1, -1, -1, -1};

	/*
	 * Just loop indefinitely waiting on hot plug event.
	 * hp_thread_handler does MMIO access, so may want to add about 500ms of thread
	 * delay between each call to hp_thread_handler.
	 */
	while (1) {
		hp_event = spdk_vmd_hotplug_handler(vmd, &addr, &is_inserted);
		if (hp_event && !is_inserted) {
			printf("Device removed at VMD pci addr %x:%x:%x.%x\n\n",
			       addr.domain, addr.bus, addr.dev, addr.func);
		}

		if (hp_event && is_inserted) {
			printf("Device inserted at VMD pci addr: %x:%x:%x.%x\n\n",
			       addr.domain, addr.bus, addr.dev, addr.func);
		}
	}

	/*
	 * Cleanup the thread upon exit.
	 */
	return NULL;
}

/*
 * Sample application that demonstrates the VMD hot insert and hot removal capability.
 * For the purpose of demonstration, this sample application uses a thread to
 * continuously sample poll for hot plug status changes  in VMD. It calls the vmd
 * hot plug handler which checks for changes to the vmd pci link status.
 * For hot inserted devices, VMD finds the devices, and if NVMe SSD, allocates
 *  BAR0 for NVMe register access. It verifies the NVMe SSD MMIO is accessible
 * after a hot insert by dumping the first 2 NVMe MMIO DWORDs.
 */
int main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	int rc = parse_args(argc, argv);
	struct vmd_adapter *vmd;
	int index, count;
	pthread_t hpt_id[24];
	int thread_err;

	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "vmd_hotplug";

	if (spdk_env_init(&opts) < 0) {
		printf("Unable to initialize SPDK env\n");
		return 1;
	}

	rc = spdk_vmd_init();

	if (rc) {
		printf("No VMD Controllers found\n");
	} else {
		/*
		 * pplications would regularly probe the vmd hot plug handler(hp_thread_handler)
		 * for hot plug detection. For this sample app, we check all vmd adapters
		 * that SPDK found for hot plug events. Create a thread to check each
		 * VMD for hot plug. An spdk application may choose to do this differently.
		 */
		count = spdk_vmd_get_count();
		if (count >= 0) {
			for (index = 0; index < count; ++index) {
				vmd = spdk_vmd_get_adapter_by_index(index);
				if (vmd == NULL) {
					continue;
				}

				thread_err = pthread_create(&hpt_id[index], NULL, hp_thread, vmd);
				if (thread_err) {
					printf("Cannot create hotplug thread\n");
					printf("%s: error = %x\n", __func__, thread_err);
				}
			}
		}


	}

	while (1);
	return rc;
}
