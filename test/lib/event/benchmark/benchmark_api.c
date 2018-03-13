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
#include "benchmark.h"

static __thread uint64_t g_bytes_processed = 0;

void
submit_callback(void *arg, benchmark_cb_fn cb_fn)
{
	struct benchmark_iov iov;
	bool last = false;

	iov.calculate_iova = false;
	while (!last) {
		last = cb_fn(arg, &iov);
		g_bytes_processed += iov.len;
	}
}

void
submit_callback_iova(void *arg, benchmark_cb_fn cb_fn)
{
	struct benchmark_iov iov;
	bool last = false;

	iov.calculate_iova = true;
	while (!last) {
		last = cb_fn(arg, &iov);
		g_bytes_processed += iov.len;
	}
}

void
submit_structure(struct benchmark_iov *iov, int iovcnt)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		g_bytes_processed += iov[i].len;
	}
}

void
submit_structure_link(struct benchmark_iov *iov)
{
	while (iov != NULL) {
		g_bytes_processed += iov->len;
		iov = iov->next;
	}
}
