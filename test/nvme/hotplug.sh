#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ -z "${DEPENDENCY_DIR}" ]; then
	echo DEPENDENCY_DIR not defined!
	exit 1
fi

function ssh_vm() {
	xtrace_disable
	sshpass -p "$password" ssh -o PubkeyAuthentication=no \
		-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 10022 root@localhost "$@"
	xtrace_restore
}

function monitor_cmd() {
	echo "$@" | nc localhost 4444 | tail --lines=+2 | (grep -v '^(qemu) ' || true)
}

function get_online_devices_count() {
	ssh_vm "lspci | grep -c NVM"
}

function wait_for_devices_ready() {
	count=$(get_online_devices_count)

	while [ $count -ne 4 ]; do
		echo "waitting for all devices online"
		count=$(get_online_devices_count)
	done
}

function insert_devices() {
	for i in {0..3}; do
		monitor_cmd "device_add nvme,drive=drive$i,id=nvme$i,serial=nvme$i"
	done
	wait_for_devices_ready
	ssh_vm "scripts/setup.sh"
}

function remove_devices() {
	for i in {0..3}; do
		monitor_cmd "device_del nvme$i"
	done
}

function devices_delete() {
	for i in {0..3}; do
		rm "$SPDK_TEST_STORAGE/nvme$i.img"
	done
}

password=$1
base_img=$HOME/spdk_test_image.qcow2
qemu_pidfile=$HOME/qemupid

if [ ! -e "$base_img" ]; then
	echo "Hotplug VM image not found; skipping test"
	exit 0
fi

timing_enter start_qemu

for i in {0..3}; do
	dd if=/dev/zero of="$SPDK_TEST_STORAGE/nvme$i.img" bs=1M count=1024
done

qemu-system-x86_64 \
	-daemonize -display none -m 8192 \
	-pidfile "$qemu_pidfile" \
	-hda "$base_img" \
	-net user,hostfwd=tcp::10022-:22 \
	-net nic \
	-cpu host \
	-smp cores=16,sockets=1 \
	--enable-kvm \
	-chardev socket,id=mon0,host=localhost,port=4444,server,nowait \
	-mon chardev=mon0,mode=readline \
	-drive format=raw,file="$SPDK_TEST_STORAGE/nvme0.img",if=none,id=drive0 \
	-drive format=raw,file="$SPDK_TEST_STORAGE/nvme1.img",if=none,id=drive1 \
	-drive format=raw,file="$SPDK_TEST_STORAGE/nvme2.img",if=none,id=drive2 \
	-drive format=raw,file="$SPDK_TEST_STORAGE/nvme3.img",if=none,id=drive3 \
	-snapshot

timing_exit start_qemu

timing_enter wait_for_vm
ssh_vm 'echo ready'
timing_exit wait_for_vm

timing_enter copy_repo
files_to_copy="scripts "
files_to_copy+="include/spdk/pci_ids.h "
files_to_copy+="build/examples/hotplug "
files_to_copy+="build/lib "

# Select which dpdk libs to copy in case we're not building with
# spdk/dpdk submodule
if [[ -n "$SPDK_RUN_EXTERNAL_DPDK" ]]; then
	files_to_copy+="-C $SPDK_RUN_EXTERNAL_DPDK/../.. dpdk/build/lib"
else
	files_to_copy+="dpdk/build/lib "
fi

(
	cd "$rootdir"
	tar -cf - $files_to_copy
) | (ssh_vm "tar -xf -")
timing_exit copy_repo

insert_devices

timing_enter hotplug_test

ssh_vm "LD_LIBRARY_PATH=/root//build/lib:/root/dpdk/build/lib:$LD_LIBRARY_PATH build/examples/hotplug -i 0 -t 25 -n 4 -r 8" &
example_pid=$!

sleep 6
remove_devices
sleep 4
insert_devices
sleep 6
remove_devices
devices_delete

timing_enter wait_for_example
wait $example_pid
timing_exit wait_for_example

trap - SIGINT SIGTERM EXIT

qemupid=$(awk '{printf $0}' "$qemu_pidfile")
kill -9 $qemupid
rm "$qemu_pidfile"

timing_exit hotplug_test
