/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_UTILS_H
#define VBDEV_OCF_UTILS_H

#include <ocf/ocf.h>

ocf_cache_mode_t ocf_get_cache_mode(const char *cache_mode);
const char *ocf_get_cache_modename(ocf_cache_mode_t mode);

/* Get cache line size in KiB units */
int ocf_get_cache_line_size(ocf_cache_t cache);

/* Get sequential cutoff policy by name */
ocf_seq_cutoff_policy ocf_get_seqcutoff_policy(const char *policy_name);
#endif
