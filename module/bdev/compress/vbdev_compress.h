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

#ifndef SPDK_VBDEV_COMPRESS_H
#define SPDK_VBDEV_COMPRESS_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"

#define LB_SIZE_4K	0x1000UL
#define LB_SIZE_512B	0x200UL

/**
 * Get the first compression bdev.
 *
 * \return the first compression bdev.
 */
struct vbdev_compress *compress_bdev_first(void);

/**
 * Get the next compression bdev.
 *
 * \param prev previous compression bdev.
 * \return the next compression bdev.
 */
struct vbdev_compress *compress_bdev_next(struct vbdev_compress *prev);

/**
 * Test to see if a compression bdev orphan exists.
 *
 * \param name The name of the compression bdev.
 * \return true if found, false if not.
 */
bool compress_has_orphan(const char *name);

/**
 * Get the name of a compression bdev.
 *
 * \param comp_bdev The compression bdev.
 * \return the name of the compression bdev.
 */
const char *compress_get_name(const struct vbdev_compress *comp_bdev);

enum compress_pmd {
	COMPRESS_PMD_AUTO = 0,
	COMPRESS_PMD_QAT_ONLY,
	COMPRESS_PMD_ISAL_ONLY,
	COMPRESS_PMD_MAX
};

int compress_set_pmd(enum compress_pmd *opts);

typedef void (*spdk_delete_compress_complete)(void *cb_arg, int bdeverrno);

/**
 * Create new compression bdev.
 *
 * \param bdev_name Bdev on which compression bdev will be created.
 * \param pm_path Path to persistent memory.
 * \param lb_size Logical block size for the compressed volume in bytes. Must be 4K or 512.
 * \return 0 on success, other on failure.
 */
int create_compress_bdev(const char *bdev_name, const char *pm_path, uint32_t lb_size);

/**
 * Delete compress bdev.
 *
 * \param bdev_name Bdev on which compression bdev will be deleted.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_compress_delete(const char *bdev_name, spdk_delete_compress_complete cb_fn,
			  void *cb_arg);

#endif /* SPDK_VBDEV_COMPRESS_H */
