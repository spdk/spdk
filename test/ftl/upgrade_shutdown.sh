#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright 2023 Solidigm All Rights Reserved
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")
source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

function cleanup() {
	trap - SIGINT SIGTERM EXIT
	rm -f "${testdir}"/file
	rm -f "${testdir}"/file.md5
	tcp_cleanup
	remove_shm
}
trap "cleanup; exit 1" SIGINT SIGTERM EXIT

export FTL_BDEV=ftl
export FTL_BASE=$1
export FTL_BASE_SIZE=$((20 * 1024))
export FTL_CACHE=$2
export FTL_CACHE_SIZE=$((5 * 1024))
export FTL_L2P_DRAM_LIMIT=$((FTL_BASE_SIZE * 10 / 100 / 1024))

tcp_target_setup

size=$((2 ** 30)) # 1GiB
seek=0
skip=0
bs=$((2 ** 20)) # 1MiB
count=$((size / bs))
iterations=2
qd=2
sums=()

# Fill FTL
for ((i = 0; i < iterations; i++)); do
	echo "Fill FTL, iteration $((i + 1))"
	tcp_dd --if=/dev/urandom --ob=ftln1 --bs="$bs" --count="$count" --qd="$qd" --seek="$seek"
	seek=$((seek + count))

	echo "Calculate MD5 checksum, iteration $((i + 1))"
	tcp_dd --ib=ftln1 --of="${testdir}/file" --bs="$bs" --count="$count" --qd="$qd" --skip="$skip"
	skip=$((skip + count))

	md5sum "${testdir}/file" > "${testdir}/file.md5"
	sums[i]=$(cut -f1 -d' ' < "${testdir}/file.md5")
done

# Get properties of chunks
$rpc_py bdev_ftl_set_property -b ftl -p verbose_mode -v true
$rpc_py bdev_ftl_get_properties -b ftl

# Enable upgrade shutdown
$rpc_py bdev_ftl_set_property -b ftl -p prep_upgrade_on_shutdown -v true

function ftl_get_properties() {
	$rpc_py bdev_ftl_get_properties -b $FTL_BDEV
}

# Validate there are utilized chunks
used=$(ftl_get_properties | jq '[.properties[] | select(.name == "cache_device") | .chunks[] | select(.utilization != 0.0)] | length')
if [[ "$used" -eq 0 ]]; then
	echo "Shutdown upgrade ERROR, excepted utilized chunks"
	exit 1
fi

# Get properties of chunks
$rpc_py bdev_ftl_set_property -b ftl -p verbose_mode -v true
$rpc_py bdev_ftl_get_properties -b ftl

# Restart for upgrading
tcp_target_shutdown
tcp_target_setup

# Get properties of chunks
$rpc_py bdev_ftl_set_property -b ftl -p verbose_mode -v true
$rpc_py bdev_ftl_get_properties -b ftl

# Validate if all chunks utilization is 0.0
used=$(ftl_get_properties | jq '[.properties[] | select(.name == "cache_device") | .chunks[] | select(.utilization != 0.0)] | length')
if [[ "$used" -ne 0 ]]; then
	echo "Shutdown upgrade ERROR, excepted only empty chunks"
	exit 1
fi

# Validate no opened bands
opened=$(ftl_get_properties | jq '[.properties[] | select(.name == "bands") | .bands[] | select(.state == "OPENED")] | length')
if [[ "$opened" -ne 0 ]]; then
	echo "Shutdown upgrade ERROR, excepted no opened band"
	exit 1
fi

function test_validate_checksum() {
	skip=0
	for ((i = 0; i < iterations; i++)); do
		echo "Validate MD5 checksum, iteration $((i + 1))"
		tcp_dd --ib=ftln1 --of="${testdir}/file" --bs="$bs" --count="$count" --qd="${qd}" --skip="$skip"
		skip=$((skip + count))

		md5sum "${testdir}/file" > "${testdir}/file.md5"
		sum=$(cut -f1 -d' ' < "${testdir}/file.md5")

		if [[ "${sums[$i]}" != "$sum" ]]; then
			echo "Wow, wow, wow!!! What is going on? MD5 Checksum ERROR!!!"
			exit 1
		fi
	done
}
test_validate_checksum

# Data integrity test after dirty shutdown after upgrade
tcp_target_shutdown_dirty
tcp_target_setup
test_validate_checksum

trap - SIGINT SIGTERM EXIT
cleanup
