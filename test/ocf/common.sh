source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

function nvme_cfg() {
	if [ -z "$ocf_nvme_cfg" ]; then
		ocf_nvme_cfg=$($rootdir/scripts/gen_nvme.sh)
	fi
	echo "$ocf_nvme_cfg"
}

function clear_nvme() {
	mapfile -t bdf < <(get_first_nvme_bdf)

	# Clear metadata on NVMe device
	$rootdir/scripts/setup.sh reset
	sleep 5

	name=$(get_nvme_name_from_bdf "${bdf[0]}")
	mountpoints=$(lsblk /dev/$name --output MOUNTPOINT -n | wc -w)
	if [ "$mountpoints" != "0" ]; then
		exit 1
	fi
	dd if=/dev/zero of=/dev/$name bs=1M count=1000 oflag=direct
	$rootdir/scripts/setup.sh
}
