/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef VHOST_FUZZ_H
#define VHOST_FUZZ_H

int fuzz_vhost_dev_init(const char *socket_path, bool is_blk_dev, bool use_bogus_buffer,
			bool use_valid_buffer, bool valid_lun, bool test_scsi_tmf);

#endif
