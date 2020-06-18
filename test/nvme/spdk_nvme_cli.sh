#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [[ $(uname) != "Linux" ]]; then
	echo "NVMe cuse tests only supported on Linux"
	exit 1
fi

if [ -z "${DEPENDENCY_DIR}" ]; then
	echo DEPENDENCY_DIR not defined!
	exit 1
fi

spdk_nvme_cli="${DEPENDENCY_DIR}/nvme-cli"

if [ ! -d $spdk_nvme_cli ]; then
	echo "nvme-cli repository not found at $spdk_nvme_cli; skipping tests."
	exit 1
fi

# Build against the version of SPDK under test
cd $spdk_nvme_cli

git clean -dfx

rm -f "$spdk_nvme_cli/spdk"
ln -sf "$rootdir" "$spdk_nvme_cli/spdk"

make -j$(nproc) LDFLAGS="$(make -s -C $spdk_nvme_cli/spdk ldflags)"

trap "kill_stub; exit 1" SIGINT SIGTERM EXIT
start_stub "-s 2048 -i 0 -m 0xF"

sed -i 's/spdk=0/spdk=1/g' spdk.conf
sed -i 's/shm_id=.*/shm_id=0/g' spdk.conf
for bdf in $(get_nvme_bdfs); do
	./nvme list
	./nvme id-ctrl $bdf
	./nvme list-ctrl $bdf
	./nvme get-ns-id $bdf
	./nvme id-ns $bdf
	./nvme fw-log $bdf
	./nvme smart-log $bdf
	./nvme error-log $bdf
	./nvme list-ns $bdf -n 1
	./nvme get-feature $bdf -f 1 -s 1 -l 100
	./nvme get-log $bdf -i 1 -l 100
	./nvme reset $bdf
done

trap - SIGINT SIGTERM EXIT
kill_stub
