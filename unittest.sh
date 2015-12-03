#!/usr/bin/env bash

set -xe

make -C test/lib/nvme/unit

test/lib/nvme/unit/nvme_c/nvme_ut
test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut
test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut
test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut

make -C test/lib/ioat/unit

test/lib/ioat/unit/ioat_ut
