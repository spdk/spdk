/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 SPDK Hot Upgrade Contributors.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/hot_upgrade.h"
#include "spdk/hot_upgrade_shared.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

/*
 * Global state for the hot upgrade subsystem
 *
 * C-4/C-5: Use __atomic_store/__atomic_load for cross-reactor visibility.
 * In POC's single-reactor run-to-completion model, plain access is safe;
 * atomic operations provide future-proofing for multi-reactor deployments.
 */
static enum spdk_hot_upgrade_state g_hu_state = SPDK_HU_IDLE;
static bool g_hu_initialized = false;
static bool g_hu_is_primary = true;
static int g_hu_ipc_sock = -1;
static int g_hu_ipc_listen_sock = -1;
static struct spdk_hot_upgrade_shared_state *g_hu_shared_state = NULL;
static int g_hu_state_fd = -1;

/*
 * SIGUSR1 handler: sets flag only. The pause() loop in
 * primary_suspend_done checks this flag and breaks.
 * After breaking, set_state(SPDK_HU_IDLE) is called in
 * the main thread context for proper state validation.
 */
static volatile sig_atomic_t g_hu_sigusr1_received = 0;

static void hu_sigusr1_handler(int signo)
{
	g_hu_sigusr1_received = 1;
}

bool
spdk_hot_upgrade_sigusr1_received(void)
{
	return g_hu_sigusr1_received != 0;
}

void
spdk_hot_upgrade_clear_sigusr1(void)
{
	g_hu_sigusr1_received = 0;
}

static const char *g_state_names[] = {
	[SPDK_HU_IDLE]                    = "IDLE",
	[SPDK_HU_SECONDARY_PRE_INIT]      = "SECONDARY_PRE_INIT",
	[SPDK_HU_SECONDARY_PRE_INIT_DONE] = "SECONDARY_PRE_INIT_DONE",
	[SPDK_HU_PRIMARY_DRAINING]        = "PRIMARY_DRAINING",
	[SPDK_HU_PRIMARY_SUSPENDED]       = "PRIMARY_SUSPENDED",
	[SPDK_HU_SECONDARY_TAKEOVER]      = "SECONDARY_TAKEOVER",
	[SPDK_HU_COMPLETE]                = "COMPLETE",
	[SPDK_HU_FAILED]                  = "FAILED",
};

void
spdk_hot_upgrade_set_state(enum spdk_hot_upgrade_state state)
{
	if (!g_hu_initialized) {
		SPDK_ERRLOG("Hot upgrade subsystem not initialized\n");
		return;
	}

	if (__atomic_load_n(&g_hu_state, __ATOMIC_ACQUIRE) == state) {
		return;
	}

	if (!spdk_hot_upgrade_state_transition_valid(
		     __atomic_load_n(&g_hu_state, __ATOMIC_ACQUIRE), state)) {
		SPDK_ERRLOG("Invalid state transition: %s -> %s\n",
			    spdk_hot_upgrade_state_str(
				    __atomic_load_n(&g_hu_state, __ATOMIC_ACQUIRE)),
			    spdk_hot_upgrade_state_str(state));
		return;
	}

	SPDK_NOTICELOG("Hot upgrade state transition: %s -> %s\n",
		       spdk_hot_upgrade_state_str(
			       __atomic_load_n(&g_hu_state, __ATOMIC_ACQUIRE)),
		       spdk_hot_upgrade_state_str(state));
	/* C-4/C-5: Atomic store with release semantics for cross-reactor visibility */
	__atomic_store_n(&g_hu_state, state, __ATOMIC_RELEASE);
}

enum spdk_hot_upgrade_state
spdk_hot_upgrade_get_state(void)
{
	/* C-4/C-5: Atomic load with acquire semantics */
	return __atomic_load_n(&g_hu_state, __ATOMIC_ACQUIRE);
}

bool
spdk_hot_upgrade_state_transition_valid(enum spdk_hot_upgrade_state from,
					enum spdk_hot_upgrade_state to)
{
	switch (from) {
	case SPDK_HU_IDLE:
		return (to == SPDK_HU_SECONDARY_PRE_INIT ||
			to == SPDK_HU_PRIMARY_DRAINING);
	case SPDK_HU_SECONDARY_PRE_INIT:
		return (to == SPDK_HU_SECONDARY_PRE_INIT_DONE ||
			to == SPDK_HU_FAILED);
	case SPDK_HU_SECONDARY_PRE_INIT_DONE:
		return (to == SPDK_HU_SECONDARY_TAKEOVER ||
			to == SPDK_HU_FAILED);
	case SPDK_HU_PRIMARY_DRAINING:
		return (to == SPDK_HU_PRIMARY_SUSPENDED ||
			to == SPDK_HU_FAILED);
	case SPDK_HU_PRIMARY_SUSPENDED:
		return (to == SPDK_HU_COMPLETE ||
			to == SPDK_HU_IDLE ||
			to == SPDK_HU_FAILED);
	case SPDK_HU_SECONDARY_TAKEOVER:
		return (to == SPDK_HU_COMPLETE ||
			to == SPDK_HU_FAILED);
	case SPDK_HU_COMPLETE:
		return false;
	case SPDK_HU_FAILED:
		return (to == SPDK_HU_IDLE);
	default:
		return false;
	}
}

const char *
spdk_hot_upgrade_state_str(enum spdk_hot_upgrade_state state)
{
	if (state < 0 || state >= (int)SPDK_COUNTOF(g_state_names)) {
		return "UNKNOWN";
	}
	return g_state_names[state];
}

int
spdk_hot_upgrade_init(void)
{
	if (g_hu_initialized) {
		return 0;
	}

	g_hu_state = SPDK_HU_IDLE;
	g_hu_initialized = true;

	/* Register SIGUSR1 handler for primary_resume wake-up */
	signal(SIGUSR1, hu_sigusr1_handler);

	SPDK_NOTICELOG("Hot upgrade subsystem initialized (role: %s)\n",
		       g_hu_is_primary ? "primary" : "secondary");
	return 0;
}

bool
spdk_hot_upgrade_is_initialized(void)
{
	return g_hu_initialized;
}

bool
spdk_hot_upgrade_is_primary(void)
{
	return g_hu_is_primary;
}

void
spdk_hot_upgrade_set_process_role(bool is_primary)
{
	g_hu_is_primary = is_primary;
}

/* ===== IPC Socket Management ===== */

int
spdk_hot_upgrade_create_ipc_sock(void)
{
	struct sockaddr_un addr;
	int rc;

	if (g_hu_ipc_listen_sock >= 0) {
		return g_hu_ipc_listen_sock;
	}

	g_hu_ipc_listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_hu_ipc_listen_sock < 0) {
		SPDK_ERRLOG("Failed to create IPC socket: %s\n", spdk_strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SPDK_HU_IPC_SOCK_PATH);

	unlink(SPDK_HU_IPC_SOCK_PATH);  /* Remove stale socket */

	rc = bind(g_hu_ipc_listen_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		SPDK_ERRLOG("Failed to bind IPC socket: %s\n", spdk_strerror(errno));
		close(g_hu_ipc_listen_sock);
		g_hu_ipc_listen_sock = -1;
		return -1;
	}

	rc = listen(g_hu_ipc_listen_sock, 1);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to listen on IPC socket: %s\n", spdk_strerror(errno));
		close(g_hu_ipc_listen_sock);
		g_hu_ipc_listen_sock = -1;
		return -1;
	}

	SPDK_NOTICELOG("IPC listen socket created at %s (fd=%d)\n",
		       SPDK_HU_IPC_SOCK_PATH, g_hu_ipc_listen_sock);
	return g_hu_ipc_listen_sock;
}

int
spdk_hot_upgrade_connect_ipc_sock(void)
{
	struct sockaddr_un addr;
	struct pollfd pfd;
	socklen_t solen;
	int rc, flags;

	if (g_hu_ipc_sock >= 0)
		return g_hu_ipc_sock;

	g_hu_ipc_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (g_hu_ipc_sock < 0) {
		SPDK_ERRLOG("Failed to create IPC client socket: %s\n", spdk_strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SPDK_HU_IPC_SOCK_PATH);

	rc = connect(g_hu_ipc_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0 && errno != EINPROGRESS) {
		SPDK_ERRLOG("Failed to connect to IPC socket: %s\n", spdk_strerror(errno));
		goto err_close;
	}

	pfd.fd = g_hu_ipc_sock;
	pfd.events = POLLOUT;
	if (poll(&pfd, 1, 5000) <= 0) {
		SPDK_ERRLOG("IPC connect timed out\n");
		goto err_close;
	}

	solen = sizeof(rc);
	if (getsockopt(g_hu_ipc_sock, SOL_SOCKET, SO_ERROR, &rc, &solen) < 0 || rc != 0) {
		SPDK_ERRLOG("IPC connect failed: %s\n", spdk_strerror(rc));
		goto err_close;
	}

	flags = fcntl(g_hu_ipc_sock, F_GETFL, 0);
	fcntl(g_hu_ipc_sock, F_SETFL, flags & ~O_NONBLOCK);

	SPDK_NOTICELOG("Connected to IPC socket %s (fd=%d)\n",
		       SPDK_HU_IPC_SOCK_PATH, g_hu_ipc_sock);
	return g_hu_ipc_sock;

err_close:
	close(g_hu_ipc_sock);
	g_hu_ipc_sock = -1;
	return -ETIMEDOUT;
}

int
spdk_hot_upgrade_get_ipc_sock(void)
{
	return g_hu_ipc_sock >= 0 ? g_hu_ipc_sock : g_hu_ipc_listen_sock;
}

/* ===== SCM_RIGHTS FD Transfer ===== */

int
spdk_hot_upgrade_send_fd(int sock, int fd)
{
	struct msghdr msg = {0};
	struct iovec iov;
	int dummy = 0;
	char buf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;

	iov.iov_base = &dummy;
	iov.iov_len = sizeof(dummy);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*(int *)CMSG_DATA(cmsg) = fd;

	if (sendmsg(sock, &msg, 0) < 0) {
		SPDK_ERRLOG("sendmsg SCM_RIGHTS failed: %s\n", spdk_strerror(errno));
		return -1;
	}

	return 0;
}

int
spdk_hot_upgrade_recv_fd(int sock)
{
	struct msghdr msg = {0};
	struct iovec iov;
	int dummy;
	char buf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	int fd;

	iov.iov_base = &dummy;
	iov.iov_len = sizeof(dummy);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);

	if (recvmsg(sock, &msg, 0) < 0) {
		SPDK_ERRLOG("recvmsg SCM_RIGHTS failed: %s\n", spdk_strerror(errno));
		return -1;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL || cmsg->cmsg_type != SCM_RIGHTS) {
		SPDK_ERRLOG("recvmsg didn't receive SCM_RIGHTS\n");
		return -1;
	}

	memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
	return fd;
}

/* ===== Shared State File Management ===== */

int
spdk_hot_upgrade_state_save(struct spdk_hot_upgrade_shared_state *state)
{
	ssize_t written;

	if (state == NULL) {
		SPDK_ERRLOG("Cannot save NULL state\n");
		return -EINVAL;
	}

	state->magic = SPDK_HU_STATE_MAGIC;
	state->version = SPDK_HU_STATE_VERSION;

	g_hu_state_fd = open(SPDK_HU_STATE_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (g_hu_state_fd < 0) {
		SPDK_ERRLOG("Failed to open state file %s: %s\n",
			    SPDK_HU_STATE_FILE, spdk_strerror(errno));
		return -errno;
	}

	if (ftruncate(g_hu_state_fd, sizeof(*state)) != 0) {
		SPDK_ERRLOG("Failed to truncate state file: %s\n", spdk_strerror(errno));
		close(g_hu_state_fd);
		g_hu_state_fd = -1;
		return -errno;
	}

	g_hu_shared_state = mmap(NULL, sizeof(*state),
				  PROT_READ | PROT_WRITE, MAP_SHARED,
				  g_hu_state_fd, 0);
	if (g_hu_shared_state == MAP_FAILED) {
		SPDK_ERRLOG("Failed to mmap state file: %s\n", spdk_strerror(errno));
		close(g_hu_state_fd);
		g_hu_state_fd = -1;
		return -errno;
	}
	memcpy(g_hu_shared_state, state, sizeof(*state));

	if (msync(g_hu_shared_state, sizeof(*state), MS_SYNC) != 0) {
		SPDK_ERRLOG("Failed to msync state file: %s\n", spdk_strerror(errno));
		return -errno;
	}

	/* P4-04: fsync to ensure file metadata (size, mtime) hits disk */
	if (fsync(g_hu_state_fd) != 0) {
		SPDK_ERRLOG("Failed to fsync state file: %s\n", spdk_strerror(errno));
		/* Non-fatal: msync already flushed data, Secondary can still mmap */
	}

	SPDK_NOTICELOG("Shared state saved to %s (%zu bytes)\n",
		       SPDK_HU_STATE_FILE, sizeof(*state));
	return 0;
}

int
spdk_hot_upgrade_state_load(struct spdk_hot_upgrade_shared_state **state)
{
	int fd;
	struct stat st;

	if (state == NULL) {
		return -EINVAL;
	}

	fd = open(SPDK_HU_STATE_FILE, O_RDWR);
	if (fd < 0) {
		SPDK_ERRLOG("Failed to open state file %s: %s\n",
			    SPDK_HU_STATE_FILE, spdk_strerror(errno));
		return -errno;
	}

	if (fstat(fd, &st) != 0) {
		SPDK_ERRLOG("Failed to stat state file: %s\n", spdk_strerror(errno));
		close(fd);
		return -errno;
	}

	if ((size_t)st.st_size < sizeof(struct spdk_hot_upgrade_shared_state)) {
		SPDK_ERRLOG("State file too small: %ld < %zu\n",
			    st.st_size, sizeof(struct spdk_hot_upgrade_shared_state));
		close(fd);
		return -EINVAL;
	}

	g_hu_shared_state = mmap(NULL, sizeof(**state),
				  PROT_READ | PROT_WRITE, MAP_SHARED,
				  fd, 0);
	if (g_hu_shared_state == MAP_FAILED) {
		SPDK_ERRLOG("Failed to mmap state file: %s\n", spdk_strerror(errno));
		close(fd);
		return -errno;
	}

	close(fd);

	if (g_hu_shared_state->magic != SPDK_HU_STATE_MAGIC) {
		SPDK_ERRLOG("Invalid state file magic: 0x%x != 0x%x\n",
			    g_hu_shared_state->magic, SPDK_HU_STATE_MAGIC);
		munmap(g_hu_shared_state, sizeof(**state));
		g_hu_shared_state = NULL;
		return -EINVAL;
	}

	*state = g_hu_shared_state;

	SPDK_NOTICELOG("Shared state loaded from %s (magic=0x%x, version=%u)\n",
		       SPDK_HU_STATE_FILE, g_hu_shared_state->magic,
		       g_hu_shared_state->version);
	return 0;
}

void
spdk_hot_upgrade_state_file_cleanup(void)
{
	/*
	 * P4-08: Prevent race with Secondary that may still be reading.
	 * Strategy: munmap first (breaks Secondary's MAP_SHARED view),
	 * then delay briefly before unlink to let any in-flight reads complete.
	 */
	if (g_hu_shared_state != NULL) {
		munmap(g_hu_shared_state, sizeof(*g_hu_shared_state));
		g_hu_shared_state = NULL;
	}

	if (g_hu_state_fd >= 0) {
		close(g_hu_state_fd);
		g_hu_state_fd = -1;
	}

	usleep(10000); /* 10ms grace period for Secondary readers */
	unlink(SPDK_HU_STATE_FILE);

	if (g_hu_ipc_sock >= 0) {
		close(g_hu_ipc_sock);
		g_hu_ipc_sock = -1;
	}

	if (g_hu_ipc_listen_sock >= 0) {
		close(g_hu_ipc_listen_sock);
		g_hu_ipc_listen_sock = -1;
	}

	unlink(SPDK_HU_IPC_SOCK_PATH);
}

SPDK_LOG_REGISTER_COMPONENT(hot_upgrade)