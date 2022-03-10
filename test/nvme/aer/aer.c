/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

#define MAX_DEVS 64

struct dev {
	struct spdk_nvme_ctrlr				*ctrlr;
	/* Expected changed NS ID state before AER */
	bool						ns_test_active;
	struct spdk_nvme_health_information_page	*health_page;
	uint32_t					orig_temp_threshold;
	char						name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static void get_feature_test(struct dev *dev);

static struct dev g_devs[MAX_DEVS];
static int g_num_devs = 0;

#define foreach_dev(iter) \
	for (iter = g_devs; iter - g_devs < g_num_devs; iter++)

static int g_outstanding_commands = 0;
static int g_aer_done = 0;
static int g_temperature_done = 0;
static int g_failed = 0;
static struct spdk_nvme_transport_id g_trid;
static char *g_touch_file;

/* Enable AER temperature test */
static int g_enable_temp_test = 0;
/* Expected changed NS ID */
static uint32_t g_expected_ns_test = 0;
/* For multi-process test */
static int g_multi_process_test = 0;
static bool g_parent_process;
static const char *g_sem_init_name = "/init";
static const char *g_sem_child_name = "/child";
static sem_t *g_sem_init_id;
static sem_t *g_sem_child_id;

static void
set_temp_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: set feature (temp threshold) failed\n", dev->name);
		g_failed = 1;
		return;
	}

	/* Admin command completions are synchronized by the NVMe driver,
	 * so we don't need to do any special locking here. */
	g_temperature_done++;
}

static int
set_temp_threshold(struct dev *dev, uint32_t temp)
{
	struct spdk_nvme_cmd cmd = {};
	int rc;

	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;
	cmd.cdw11_bits.feat_temp_threshold.bits.tmpth = temp;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, set_temp_completion, dev);
	if (rc == 0) {
		g_outstanding_commands++;
	} else {
		fprintf(stderr, "Submitting Admin cmd failed with rc: %d (%s)\n", \
			rc, (g_parent_process ? "Parent" : "Child"));
	}

	return rc;
}

static void
get_temp_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get feature (temp threshold) failed\n", dev->name);
		g_failed = 1;
		return;
	}

	dev->orig_temp_threshold = cpl->cdw0;
	printf("%s: original temperature threshold: %u Kelvin (%d Celsius)\n",
	       dev->name, dev->orig_temp_threshold, dev->orig_temp_threshold - 273);

	g_temperature_done++;
}

static int
get_temp_threshold(struct dev *dev)
{
	struct spdk_nvme_cmd cmd = {};
	int rc;

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, get_temp_completion, dev);
	if (rc == 0) {
		g_outstanding_commands++;
	}

	return rc;
}

static void
print_health_page(struct dev *dev, struct spdk_nvme_health_information_page *hip)
{
	printf("%s: Current Temperature:         %u Kelvin (%d Celsius)\n",
	       dev->name, hip->temperature, hip->temperature - 273);
}

static void
get_health_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get log page failed\n", dev->name);
		g_failed = 1;
		return;
	}

	print_health_page(dev, dev->health_page);
	g_aer_done++;
}

static int
get_health_log_page(struct dev *dev)
{
	int rc;

	rc = spdk_nvme_ctrlr_cmd_get_log_page(dev->ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
					      SPDK_NVME_GLOBAL_NS_TAG, dev->health_page, sizeof(*dev->health_page), 0,
					      get_health_log_page_completion, dev);

	if (rc == 0) {
		g_outstanding_commands++;
	}

	return rc;
}

static void
get_ns_state_test(struct dev *dev, uint32_t nsid)
{
	bool new_ns_state;

	new_ns_state = spdk_nvme_ctrlr_is_active_ns(dev->ctrlr, nsid);
	if (new_ns_state == dev->ns_test_active) {
		g_failed = 1;
	}
}

static void
cleanup(void)
{
	struct dev *dev;

	foreach_dev(dev) {
		if (dev->health_page) {
			spdk_free(dev->health_page);
		}
	}
}

static void
aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev				*dev = arg;
	uint32_t				log_page_id;
	uint32_t				aen_event_info;
	uint32_t				aen_event_type;
	union spdk_nvme_async_event_completion	aen_cpl;

	aen_cpl.raw = cpl->cdw0;
	aen_event_info = aen_cpl.bits.async_event_info;
	aen_event_type = aen_cpl.bits.async_event_type;
	log_page_id = aen_cpl.bits.log_page_identifier;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "%s: AER failed\n", dev->name);
		g_failed = 1;
		return;
	}

	printf("%s: aer_cb for log page %d, aen_event_type: 0x%02x, aen_event_info: 0x%02x (%s)\n", \
	       dev->name, log_page_id, aen_event_type, aen_event_info, \
	       (g_parent_process ? "Parent" : "Child"));
	/* Temp Test: Verify proper EventType, Event Info and Log Page.
	 * NOTE: QEMU NVMe controllers return Spare Below Threshold Status event info
	 * instead of Temperate Threshold even info which is why it's used in the check
	 * below.
	 */
	if ((log_page_id == SPDK_NVME_LOG_HEALTH_INFORMATION) && \
	    (aen_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_SMART) && \
	    ((aen_event_info == SPDK_NVME_ASYNC_EVENT_TEMPERATURE_THRESHOLD) || \
	     (aen_event_info == SPDK_NVME_ASYNC_EVENT_SPARE_BELOW_THRESHOLD))) {
		/* Set the temperature threshold back to the original value to stop triggering  */
		printf("aer_cb - Resetting Temp Threshold for device: %s (%s)\n", \
		       dev->name, (g_parent_process ? "Parent" : "Child"));
		set_temp_threshold(dev, dev->orig_temp_threshold);
		get_health_log_page(dev);
	} else if (log_page_id == SPDK_NVME_LOG_CHANGED_NS_LIST) {
		printf("aer_cb - Changed Namespace (%s)\n", \
		       (g_parent_process ? "Parent" : "Child"));
		get_ns_state_test(dev, g_expected_ns_test);
		g_aer_done++;
	} else {
		printf("aer_cb - Unknown Log Page (%s)\n", \
		       (g_parent_process ? "Parent" : "Child"));
	}
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -g         use single file descriptor for DPDK memory segments]\n");
	printf(" -T         enable temperature tests\n");
	printf(" -n         expected Namespace attribute notice ID\n");
	printf(" -t <file>  touch specified file when ready to receive AER\n");
	printf(" -r trid    remote NVMe over Fabrics target address\n");
	printf("    Format: 'key:value [key:value] ...'\n");
	printf("    Keys:\n");
	printf("     trtype      Transport type (e.g. RDMA)\n");
	printf("     adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("     traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("     trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("     subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("    Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");

	spdk_log_usage(stdout, "-L");

	printf(" -i <id>    shared memory group ID\n");
	printf(" -m         Multi-Process AER Test (only with Temp Test)\n");
	printf(" -H         show this usage\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;
	long int val;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "gi:mn:r:t:HL:T")) != -1) {
		switch (op) {
		case 'n':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Invalid NS attribute notice ID\n");
				return val;
			}
			g_expected_ns_test = (uint32_t)val;
			break;
		case 'g':
			env_opts->hugepage_single_segments = true;
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 't':
			g_touch_file = optarg;
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case 'T':
			g_enable_temp_test = 1;
			break;
		case 'H':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'i':
			env_opts->shm_id = spdk_strtol(optarg, 10);
			if (env_opts->shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return env_opts->shm_id;
			}
			break;
		case 'm':
			g_multi_process_test = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;

	/* add to dev list */
	dev = &g_devs[g_num_devs++];

	dev->ctrlr = ctrlr;

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);

	dev->health_page = spdk_zmalloc(sizeof(*dev->health_page), 4096, NULL, SPDK_ENV_LCORE_ID_ANY,
					SPDK_MALLOC_DMA);
	if (dev->health_page == NULL) {
		printf("Allocation error (health page)\n");
		g_failed = 1;
	}
}

static void
get_feature_test_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get number of queues failed\n", dev->name);
		g_failed = 1;
		return;
	}

	if (g_aer_done < g_num_devs) {
		/*
		 * Resubmit Get Features command to continue filling admin queue
		 * while the test is running.
		 */
		get_feature_test(dev);
	}
}

static void
get_feature_test(struct dev *dev)
{
	struct spdk_nvme_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;
	if (spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0,
					  get_feature_test_cb, dev) != 0) {
		printf("Failed to send Get Features command for dev=%p\n", dev);
		g_failed = 1;
		return;
	}

	g_outstanding_commands++;
}

static int
spdk_aer_temperature_test(void)
{
	struct dev *dev;

	printf("Getting orig temperature thresholds of all controllers (%s)\n",
	       (g_parent_process ? "Parent" : "Child"));
	foreach_dev(dev) {
		/* Get the original temperature threshold */
		get_temp_threshold(dev);
	}

	while (!g_failed && g_temperature_done < g_num_devs) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	if (g_failed) {
		return g_failed;
	}
	g_temperature_done = 0;
	g_aer_done = 0;

	/* Send admin commands to test admin queue wraparound while waiting for the AER */
	foreach_dev(dev) {
		get_feature_test(dev);
	}

	if (g_failed) {
		return g_failed;
	}

	/* Only single process needs to set and verify lower threshold */
	if (g_parent_process) {
		/* Wait until child has init'd and ready for test to continue */
		if (g_multi_process_test) {
			sem_wait(g_sem_child_id);
		}
		printf("Setting all controllers temperature threshold low to trigger AER\n");
		foreach_dev(dev) {
			/* Set the temperature threshold to a low value */
			set_temp_threshold(dev, 200);
		}

		printf("Waiting for all controllers temperature threshold to be set lower\n");
		while (!g_failed && (g_temperature_done < g_num_devs)) {
			foreach_dev(dev) {
				spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
			}
		}
		g_temperature_done = 0;

		if (g_failed) {
			return g_failed;
		}
	}

	printf("Waiting for all controllers to trigger AER and reset threshold (%s)\n",
	       (g_parent_process ? "Parent" : "Child"));
	/* Let parent know init is done and it's okay to continue */
	if (!g_parent_process) {
		sem_post(g_sem_child_id);
	}
	/* Waiting for AEN to be occur here */
	while (!g_failed && (g_aer_done < g_num_devs || g_temperature_done < g_num_devs)) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	if (g_failed) {
		return g_failed;
	}

	return 0;
}

static int
spdk_aer_changed_ns_test(void)
{
	struct dev *dev;

	g_aer_done = 0;

	printf("Starting namespace attribute notice tests for all controllers...\n");

	foreach_dev(dev) {
		get_feature_test(dev);
		dev->ns_test_active = spdk_nvme_ctrlr_is_active_ns(dev->ctrlr, g_expected_ns_test);
	}

	if (g_failed) {
		return g_failed;
	}

	while (!g_failed && (g_aer_done < g_num_devs)) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	if (g_failed) {
		return g_failed;
	}

	return 0;
}

static int
setup_multi_process(void)
{
	pid_t pid;
	int rc = 0;

	/* If AEN test was killed, remove named semaphore to start again */
	rc = sem_unlink(g_sem_init_name);
	if (rc < 0 && errno != ENOENT) {
		fprintf(stderr, "Init semaphore removal failure: %s", spdk_strerror(errno));
		return rc;
	}
	rc = sem_unlink(g_sem_child_name);
	if (rc < 0 && errno != ENOENT) {
		fprintf(stderr, "Child semaphore removal failure: %s", spdk_strerror(errno));
		return rc;
	}
	pid = fork();
	if (pid == -1) {
		perror("Failed to fork\n");
		return -1;
	} else if (pid == 0) {
		printf("Child process pid: %d\n", getpid());
		g_parent_process = false;
		g_sem_init_id = sem_open(g_sem_init_name, O_CREAT, 0600, 0);
		g_sem_child_id = sem_open(g_sem_child_name, O_CREAT, 0600, 0);
		if ((g_sem_init_id == SEM_FAILED) || (g_sem_child_id == SEM_FAILED)) {
			fprintf(stderr, "Sem Open failed for child: %s\n", spdk_strerror(errno));
			return -1;
		}
	}
	/* Parent process */
	else {
		g_parent_process = true;
		g_sem_init_id = sem_open(g_sem_init_name, O_CREAT, 0600, 0);
		g_sem_child_id = sem_open(g_sem_child_name, O_CREAT, 0600, 0);
		if ((g_sem_init_id == SEM_FAILED) || (g_sem_child_id == SEM_FAILED)) {
			fprintf(stderr, "Sem Open failed for parent: %s\n", spdk_strerror(errno));
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct dev		*dev;
	struct spdk_env_opts	opts;
	int			rc;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	if (g_multi_process_test)  {
		/* Multi-Process test only available with Temp Test */
		if (!g_enable_temp_test) {
			fprintf(stderr, "Multi Process test only available with Temp Test (-T)\n");
			return 1;
		}
		if (opts.shm_id < 0) {
			fprintf(stderr, "Multi Process requires shared memory id (-i <id>)\n");
			return 1;
		}
		rc = setup_multi_process();
		if (rc != 0) {
			fprintf(stderr, "Multi Process test failed to setup\n");
			return rc;
		}
	} else {
		/* Only one process in test, set it to the parent process */
		g_parent_process = true;
	}
	opts.name = "aer";
	if (g_parent_process) {
		opts.core_mask = "0x1";
	} else {
		opts.core_mask = "0x2";
	}

	/*
	 * For multi-process test, parent (primary) and child (secondary) processes
	 * will execute all following code but DPDK setup is serialized
	 */
	if (!g_parent_process) {
		if (sem_wait(g_sem_init_id) < 0) {
			fprintf(stderr, "sem_wait failed for child process\n");
			return (-1);
		}
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Asynchronous Event Request test (%s)\n",
	       (g_parent_process ? "Parent" : "Child"));

	if (spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_num_devs == 0) {
		fprintf(stderr, "No controllers found - exiting\n");
		g_failed = 1;
	}
	if (g_failed) {
		goto done;
	}

	if (g_parent_process && g_enable_temp_test) {
		printf("Reset controller to setup AER completions for this process\n");
		foreach_dev(dev) {
			if (spdk_nvme_ctrlr_reset(dev->ctrlr) < 0) {
				fprintf(stderr, "nvme reset failed.\n");
				return -1;
			}
		}
	}
	if (g_parent_process && g_multi_process_test) {
		/* Primary can release child/secondary for init now */
		sem_post(g_sem_init_id);
	}

	printf("Registering asynchronous event callbacks...\n");
	foreach_dev(dev) {
		spdk_nvme_ctrlr_register_aer_callback(dev->ctrlr, aer_cb, dev);
	}

	if (g_touch_file) {
		int fd;

		fd = open(g_touch_file, O_CREAT | O_EXCL | O_RDWR, S_IFREG);
		if (fd == -1) {
			fprintf(stderr, "Could not touch %s (%s).\n", g_touch_file, strerror(errno));
			g_failed = true;
			goto done;
		}
		close(fd);
	}

	/* AER temperature test */
	if (g_enable_temp_test) {
		if (spdk_aer_temperature_test()) {
			goto done;
		}
	}

	/* AER changed namespace list test */
	if (g_expected_ns_test) {
		if (spdk_aer_changed_ns_test()) {
			goto done;
		}
	}

	printf("Cleaning up...(%s)\n", (g_parent_process ? "Parent" : "Child"));

	while (g_outstanding_commands) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	/* Only one process cleans up at a time - let child go first */
	if (g_multi_process_test && g_parent_process) {
		/* Parent waits for child to clean up before executing clean up process */
		sem_wait(g_sem_child_id);
	}
	/* unregister AER callback so we don't fail on aborted AERs when we close out qpairs. */
	foreach_dev(dev) {
		spdk_nvme_ctrlr_register_aer_callback(dev->ctrlr, NULL, NULL);
	}

	foreach_dev(dev) {
		spdk_nvme_detach_async(dev->ctrlr, &detach_ctx);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	/* Release semaphore to allow parent to cleanup */
	if (!g_parent_process) {
		sem_post(g_sem_child_id);
		sem_wait(g_sem_init_id);
	}
done:
	cleanup();

	/* Wait for child process to finish and verify it finished correctly before detaching resources */
	if (g_multi_process_test && g_parent_process) {
		int status;
		sem_post(g_sem_init_id);
		wait(&status);
		if (WIFEXITED(status)) {
			/* Child ended normally */
			if (WEXITSTATUS(status) != 0) {
				fprintf(stderr, "Child Failed with status: %d.\n", (int8_t)(WEXITSTATUS(status)));
				g_failed = true;
			}
		}
		if (sem_close(g_sem_init_id) != 0) {
			perror("sem_close Failed for init\n");
			g_failed = true;
		}
		if (sem_close(g_sem_child_id) != 0) {
			perror("sem_close Failed for child\n");
			g_failed = true;
		}

		if (sem_unlink(g_sem_init_name) != 0) {
			perror("sem_unlink Failed for init\n");
			g_failed = true;
		}
		if (sem_unlink(g_sem_child_name) != 0) {
			perror("sem_unlink Failed for child\n");
			g_failed = true;
		}
	}
	return g_failed;
}
