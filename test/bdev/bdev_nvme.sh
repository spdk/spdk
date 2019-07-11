#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

function get_number_of_namespaces {
	names=( $(get_nvme_name_from_bdf $1) )
	printf '%d\n' ${#names[@]}
}

if [ $(uname) = Linux ]; then
	sleep 1
	$rootdir/scripts/setup.sh reset
	sleep 3

	for bdf in $(iter_pci_class_code 01 08 02); do
		bdf_str=$(tr -d .: <<< $bdf)
		ns_count=$(get_number_of_namespaces $bdf)
		declare -i ns_count_${bdf_str}=$ns_count
	done

	$rootdir/scripts/setup.sh

	$testdir/../../app/spdk_tgt/spdk_tgt &
	spdk_pid=$!
	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	echo $spdk_pid > $BASE_DIR/spdk.pid
	waitforlisten $spdk_pid

	for bdf in $(iter_pci_class_code 01 08 02); do
		$rpc_py construct_nvme_bdev -t PCIe -a $bdf -b $bdf

		# Check if controller has been created successfully
		if ! grep -q $bdf <<< $(scripts/rpc.py get_nvme_controllers); then
			exit 1
		fi

		# Check if number of created bdevs is the same as number of existing namespaces
		bdf_str=$(tr -d .: <<< $bdf)
		var="ns_count_${bdf_str}"
		namespaces=$($rpc_py get_bdevs | grep "$bdf" | grep -c name || true)
		if [ $namespaces != ${!var} ]; then
			exit 1
		fi

		# Check if each namespace/bdev details is propagated correctly for NVMe with multiple namespaces
		if [ $namespaces -gt 1 ]; then
			declare -i i=1

			while [ $i -lt $namespaces ]; do
				ns1=$($rpc_py get_bdevs -b ${bdf}n${i} | jq -r ".[] | del(.name, .uuid, .driver_specific.nvme.ns_data.id)")
				ns2=$($rpc_py get_bdevs -b ${bdf}n${i+1} | jq -r ".[] | del(.name, .uuid, .driver_specific.nvme.ns_data.id)")
				if [ $(jq -n --argjson a "$ns1" --argjson b "$ns2" '$a == $b') == false ]; then
				        exit 1
				fi

				i=$i+1
			done
		fi
	done

	trap - SIGINT SIGTERM EXIT

	if pkill -F $BASE_DIR/spdk.pid; then
		sleep 1
	fi
	rm $BASE_DIR/spdk.pid || true
fi
