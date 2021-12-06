#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
source "$rootdir/test/common/autotest_common.sh"

restore_pci_blocked() {
	[[ -n $PCI_ZONED ]] || return 0

	PCI_BLOCKED="" "$rootdir/scripts/setup.sh" reset
	PCI_BLOCKED=$PCI_ZONED "$rootdir/scripts/setup.sh"
}

gen_json_conf() {
	cat <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "method": "bdev_nvme_attach_controller",
		          "params": {
		            "trtype": "PCIe",
		            "name":"$bdev",
		            "traddr":"$bdf"
		          }
		        }
		      ]
		    }
		  ]
		}
	JSON
}

gen_fio_conf() {
	local zone_bdev

	cat <<- FIO
		[global]
		ioengine=spdk_bdev
		thread=1
		direct=1
		time_based
		runtime=5
		rw=randwrite
		bs=16K
		zonemode=zbd
		max_open_zones=8
		initial_zone_reset=1
		zone_append=1
		iodepth=64
	FIO

	for zone_bdev in "${!zoned_bdevs[@]}"; do
		cat <<- FIO
			[filename$zone_bdev]
			filename=${zoned_bdevs[zone_bdev]}
		FIO
	done
}

is_zoned() {
	# At least one namespace must be zoned
	((${#zoned_bdevs[@]} > 0))
}

fio() {
	fio_bdev --ioengine=spdk_bdev --spdk_json_conf <(gen_json_conf) <(gen_fio_conf)
}

zoned_bdfs=($PCI_ZONED)
if ((${#zoned_bdfs[@]} == 0)); then
	printf 'No ZNS nvme devices found, skipping\n' >&2
	exit 0
fi

PCI_BLOCKED="" PCI_ALLOWED="${zoned_bdfs[*]}" "$rootdir/scripts/setup.sh"
bdf=${zoned_bdfs[0]} bdev=zone0

trap 'kill $spdk_app_pid || :; restore_pci_blocked' EXIT

"${SPDK_APP[@]}" &
spdk_app_pid=$!
waitforlisten "$spdk_app_pid"

rpc_cmd bdev_nvme_attach_controller -t pcie -a "$bdf" -b "$bdev"
zoned_bdevs=($(rpc_cmd bdev_get_bdevs | jq -r ".[] | select(.zoned == true) | select(.driver_specific.nvme[].pci_address == \"$bdf\") | .name"))

killprocess "$spdk_app_pid"

run_test "is_zoned" is_zoned
run_test "zoned_fio" fio
