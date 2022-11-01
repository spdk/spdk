/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_USDT_H
#define SPDK_INTERNAL_USDT_H

#include "spdk/config.h"
#include "spdk/env.h"

#if defined(SPDK_CONFIG_USDT) && !defined(SPDK_UNIT_TEST)

#if defined(__aarch64__)
#define STAP_SDT_ARG_CONSTRAINT        nr
#endif

#include <sys/sdt.h>

#define SPDK_DTRACE_PROBE(name)			DTRACE_PROBE1(spdk,name,spdk_get_ticks())
#define SPDK_DTRACE_PROBE1(name,a1)		DTRACE_PROBE2(spdk,name,spdk_get_ticks(),a1)
#define SPDK_DTRACE_PROBE2(name,a1,a2)		DTRACE_PROBE3(spdk,name,spdk_get_ticks(),a1,a2)
#define SPDK_DTRACE_PROBE3(name,a1,a2,a3)	DTRACE_PROBE4(spdk,name,spdk_get_ticks(),a1,a2,a3)
#define SPDK_DTRACE_PROBE4(name,a1,a2,a3,a4)	DTRACE_PROBE5(spdk,name,spdk_get_ticks(),a1,a2,a3,a4)

#else

#define SPDK_DTRACE_PROBE(...)
#define SPDK_DTRACE_PROBE1(...)
#define SPDK_DTRACE_PROBE2(...)
#define SPDK_DTRACE_PROBE3(...)
#define SPDK_DTRACE_PROBE4(...)

#endif

#endif /* SPDK_INTERNAL_USDT_H */
