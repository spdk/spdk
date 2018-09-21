#!/usr/bin/env bash

set -ex

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [ -z "${DEPENDENCY_DIR}" ]; then
        echo DEPENDENCY_DIR not defined!
        exit 1
fi

spdk_nvme_cli="${DEPENDENCY_DIR}/nvme-cli"

if [ ! -d $spdk_nvme_cli ]; then
	echo "nvme-cli repository not found at $spdk_nvme_cli; skipping tests."
	exit 0
fi

timing_enter nvme_cli

if [ `uname` = Linux ]; then
	start_stub "-s 2048 -i 0 -m 0xF"
	trap "kill_stub; exit 1" SIGINT SIGTERM EXIT
fi

# Build against the version of SPDK under test
rm -f "$spdk_nvme_cli/spdk"
ln -sf "$rootdir" "$spdk_nvme_cli/spdk"

bdfs=$(iter_pci_class_code 01 08 02)
bdf=$(echo $bdfs|awk '{ print $1 }')

if [[ -s $rootdir/mk/cofnig.mk ]] && grep CONFIG_LOG_BACKTRACE $rootdir/mk/config.mk -q; then
	nvme_cli_ldflags='LDFLAGS=-lunwind'
fi

#
# HACK: revert this change when https://review.gerrithub.io/#/c/spdk/nvme-cli/+/426695/ is merged
#
echo "WARNING: Patching nvme-cli
git reset --hard
cat << 'EOF' | git apply
From 467da3f4ac2dc438ec66510814cadc1bf51a1df4 Mon Sep 17 00:00:00 2001
From: Pawel Wodkowski <pawelx.wodkowski@intel.com>
Date: Tue, 25 Sep 2018 18:03:37 +0200
Subject: [PATCH] env: switch to use mk/config.mk

Signed-off-by: Pawel Wodkowski <pawelx.wodkowski@intel.com>
---
 env.spdk.mk | 3 +--
 1 file changed, 1 insertion(+), 2 deletions(-)

diff --git a/env.spdk.mk b/env.spdk.mk
index c02f67402ef5..7b6dfcfc0d58 100644
--- a/env.spdk.mk
+++ b/env.spdk.mk
@@ -35,8 +35,7 @@ SPDK_ROOT_DIR ?= $(abspath $(CURDIR)/spdk)
 SPDK_LIB_DIR ?= $(SPDK_ROOT_DIR)/build/lib
 DPDK_LIB_DIR ?= $(SPDK_ROOT_DIR)/dpdk/build/lib

--include $(SPDK_ROOT_DIR)/CONFIG.local
-include $(SPDK_ROOT_DIR)/CONFIG
+include $(SPDK_ROOT_DIR)/mk/config.mk

 override CFLAGS += -I$(SPDK_ROOT_DIR)/include
 override LDFLAGS += \
--
2.7.4
EOF

cd $spdk_nvme_cli
make clean && make -j$(nproc) $nvme_cli_ldflags
sed -i 's/spdk=0/spdk=1/g' spdk.conf
sed -i 's/shm_id=1/shm_id=0/g' spdk.conf
./nvme list
./nvme id-ctrl $bdf
./nvme list-ctrl $bdf
./nvme get-ns-id $bdf
./nvme id-ns $bdf
./nvme fw-log $bdf
./nvme smart-log $bdf
./nvme error-log $bdf
./nvme list-ns $bdf -n 1
./nvme get-feature $bdf -n 1 -f 1 -s 1 -l 100
./nvme get-log $bdf -n 1 -i 1 -l 100
./nvme reset $bdf
if [ `uname` = Linux ]; then
	trap - SIGINT SIGTERM EXIT
	kill_stub
fi

report_test_completion spdk_nvme_cli
timing_exit nvme_cli
