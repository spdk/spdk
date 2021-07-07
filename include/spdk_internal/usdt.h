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

#ifndef SPDK_INTERNAL_USDT_H
#define SPDK_INTERNAL_USDT_H

#include "spdk/config.h"
#include "spdk/env.h"

#if defined(SPDK_CONFIG_USDT) && !defined(SPDK_UNIT_TEST)

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
