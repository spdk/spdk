#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [[ $(uname) != "Linux" ]]; then
	echo "NVMe cuse tests only supported on Linux"
	exit 1
fi

nvme_cli_build

trap "kill_stub; exit 1" SIGINT SIGTERM EXIT
start_stub "-s 2048 -i 0 -m 0xF"

pushd ${DEPENDENCY_DIR}/nvme-cli

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

popd

trap - SIGINT SIGTERM EXIT
kill_stub
