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

#ifndef SPDK_VBDEV_SPLIT_H
#define SPDK_VBDEV_SPLIT_H

#include "spdk/bdev_module.h"

/**
 * Add given disk name to split config. If bdev with \c base_bdev_name name
 * exist the split bdevs will be created right away, if not the split bdevs will
 * be created when base bdev became be available (during examination process).
 *
 * \param base_bdev_name Base bdev name
 * \param split_count number of splits to be created.
 * \param split_size_mb size of each bdev. If 0 use base bdev size / split_count
 * \return value >= 0 - number of splits create. Negative errno code on error.
 */
int create_vbdev_split(const char *base_bdev_name, unsigned split_count, uint64_t split_size_mb);

/**
 * Remove all created split bdevs and split config.
 *
 * \param base_bdev_name base bdev name
 * \return 0 on success or negative errno value.
 */
int vbdev_split_destruct(const char *base_bdev_name);

/**
 * Get the spdk_bdev_part_base associated with the given split base_bdev.
 *
 * \param base_bdev Bdev to get the part_base from
 * \return pointer to the associated spdk_bdev_part_base
 * \return NULL if the base_bdev is not being split by the split module
 */
struct spdk_bdev_part_base *vbdev_split_get_part_base(struct spdk_bdev *base_bdev);

#endif /* SPDK_VBDEV_SPLIT_H */
