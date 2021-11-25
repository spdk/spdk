#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

SMARTCTL_CMD='smartctl -d nvme'
rpc_py=$rootdir/scripts/rpc.py

bdf=$(get_first_nvme_bdf)

PCI_ALLOWED="${bdf}" $rootdir/scripts/setup.sh reset
nvme_name=$(get_nvme_ctrlr_from_bdf ${bdf})
if [[ -z "$nvme_name" ]]; then
	echo "setup.sh failed bind kernel driver to ${bdf}"
	exit 1
fi

KERNEL_SMART_JSON=$(${SMARTCTL_CMD} --json=g -a /dev/${nvme_name} | grep -v "/dev/${nvme_name}" | sort || true)

${SMARTCTL_CMD} -i /dev/${nvme_name}n1

# logs are not provided by json output
KERNEL_SMART_ERRLOG=$(${SMARTCTL_CMD} -l error /dev/${nvme_name})

$rootdir/scripts/setup.sh

$SPDK_BIN_DIR/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py bdev_nvme_cuse_register -n Nvme0

sleep 5

if [ ! -c /dev/spdk/nvme0 ]; then
	exit 1
fi

CUSE_SMART_JSON=$(${SMARTCTL_CMD} --json=g -a /dev/spdk/nvme0 | grep -v "/dev/spdk/nvme0" | sort || true)

DIFF_SMART_JSON=$(diff --changed-group-format='%<' --unchanged-group-format='' <(echo "$KERNEL_SMART_JSON") <(echo "$CUSE_SMART_JSON") || true)

# Mask values can change
ERR_SMART_JSON=$(grep -v "json\.nvme_smart_health_information_log\.\|json\.local_time\.\|json\.temperature\.\|json\.power_on_time\.hours" <<< $DIFF_SMART_JSON || true)

if [ -n "$ERR_SMART_JSON" ]; then
	echo "Wrong values for: $ERR_SMART_JSON"
	exit 1
fi

CUSE_SMART_ERRLOG=$(${SMARTCTL_CMD} -l error /dev/spdk/nvme0)
if [ "$CUSE_SMART_ERRLOG" != "$KERNEL_SMART_ERRLOG" ]; then
	echo "Wrong values in NVMe Error log"
	exit 1
fi

# Data integrity was checked before, now make sure other commads didn't fail
${SMARTCTL_CMD} -i /dev/spdk/nvme0n1
${SMARTCTL_CMD} -c /dev/spdk/nvme0
${SMARTCTL_CMD} -A /dev/spdk/nvme0

# Health test can fail
${SMARTCTL_CMD} -x /dev/spdk/nvme0 || true
${SMARTCTL_CMD} -H /dev/spdk/nvme0 || true

$rpc_py bdev_nvme_detach_controller Nvme0
sleep 1
if [ -c /dev/spdk/nvme1 ]; then
	exit 1
fi

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
