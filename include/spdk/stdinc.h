/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Standard C headers
 *
 * This file is intended to be included first by all other SPDK files.
 */

#ifndef SPDK_STDINC_H
#define SPDK_STDINC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Standard C */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* POSIX */
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <ifaddrs.h>
#include <libgen.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <regex.h>

/* GNU extension */
#include <getopt.h>

/* Linux */
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPDK_STDINC_H */
