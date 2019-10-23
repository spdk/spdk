#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [ -z "${DEPENDENCY_DIR}" ]; then
        echo DEPENDENCY_DIR not defined!
        exit 1
fi

timing_enter nvme_cli

if [ $SPDK_TEST_NVME_CUSE -eq 1 ]; then

	NVME_CMD=/usr/local/src/nvme-cli/nvme
	rpc_py=$rootdir/scripts/rpc.py

	$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 &
	spdk_tgt_pid=$!
	trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

	waitforlisten $spdk_tgt_pid

	bdf=$(iter_pci_class_code 01 08 02 | head -1)
	i=0

	$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
	$rpc_py nvme_cuse_register -t PCIe -a ${bdf} -p spdk/nvme0

	sleep 5

	for ns in $(ls /dev/spdk/nvme?n?); do
		${NVME_CMD} get-ns-id $ns
		${NVME_CMD} id-ns $ns
		${NVME_CMD} list-ns $ns
	done

	for ctrlr in $(ls /dev/spdk/nvme?); do
		${NVME_CMD} id-ctrl $ctrlr
		${NVME_CMD} list-ctrl $ctrlr
		${NVME_CMD} fw-log $ctrlr
		${NVME_CMD} smart-log $ctrlr
		${NVME_CMD} error-log $ctrlr
		${NVME_CMD} get-feature $ctrlr -f 1 -s 1 -l 100
		${NVME_CMD} get-log $ctrlr -i 1 -l 100
		${NVME_CMD} reset $ctrlr
	done

	trap - SIGINT SIGTERM EXIT
	kill $spdk_tgt_pid
else
	spdk_nvme_cli="${DEPENDENCY_DIR}/nvme-cli"

	if [ ! -d $spdk_nvme_cli ]; then
		echo "nvme-cli repository not found at $spdk_nvme_cli; skipping tests."
		exit 0
	fi

	if [ $(uname) = Linux ]; then
		start_stub "-s 2048 -i 0 -m 0xF"
		trap "kill_stub; exit 1" SIGINT SIGTERM EXIT
	fi

	# Build against the version of SPDK under test
	rm -f "$spdk_nvme_cli/spdk"
	ln -sf "$rootdir" "$spdk_nvme_cli/spdk"

	cd $spdk_nvme_cli
	make clean && make -j$(nproc) LDFLAGS="$(make -s -C $spdk_nvme_cli/spdk ldflags)"
	sed -i 's/spdk=0/spdk=1/g' spdk.conf
	sed -i 's/shm_id=.*/shm_id=0/g' spdk.conf
	for bdf in $(iter_pci_class_code 01 08 02); do
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

	if [ $(uname) = Linux ]; then
		trap - SIGINT SIGTERM EXIT
		kill_stub
	fi
fi

report_test_completion spdk_nvme_cli
timing_exit nvme_cli
