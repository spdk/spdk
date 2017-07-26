#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

function ssh_vm() {
	sshpass -p "$password" ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no -p 10022 root@localhost "$@"
}

function monitor_cmd() {
	rc=0
	if ! (echo "$@" | nc localhost 4444 > mon.log); then
		rc=1
		cat mon.log
	fi
	rm mon.log
	return $rc
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

function devices_initialization() {
	timing_enter devices_initialization
	dd if=/dev/zero of=/root/test0 bs=1M count=1024
	dd if=/dev/zero of=/root/test1 bs=1M count=1024
	dd if=/dev/zero of=/root/test2 bs=1M count=1024
	dd if=/dev/zero of=/root/test3 bs=1M count=1024
	monitor_cmd "drive_add 0 file=/root/test0,format=raw,id=drive0,if=none"
	monitor_cmd "drive_add 1 file=/root/test1,format=raw,id=drive1,if=none"
	monitor_cmd "drive_add 2 file=/root/test2,format=raw,id=drive2,if=none"
	monitor_cmd "drive_add 3 file=/root/test3,format=raw,id=drive3,if=none"
	timing_exit devices_initialization
}

function insert_devices() {
	monitor_cmd "device_add nvme,drive=drive0,id=nvme0,serial=nvme0"
	monitor_cmd "device_add nvme,drive=drive1,id=nvme1,serial=nvme1"
	monitor_cmd "device_add nvme,drive=drive2,id=nvme2,serial=nvme2"
	monitor_cmd "device_add nvme,drive=drive3,id=nvme3,serial=nvme3"
	wait_for_devices_ready
	ssh_vm "scripts/setup.sh"
}

function remove_devices() {
	monitor_cmd "device_del nvme0"
	monitor_cmd "device_del nvme1"
	monitor_cmd "device_del nvme2"
	monitor_cmd "device_del nvme3"
}

function devices_delete() {
	timing_enter devices_delete
	rm /root/test0
	rm /root/test1
	rm /root/test2
	rm /root/test3
	timing_exit devices_delete
}

password=$1
base_img=/home/sys_sgsw/fedora24.img
test_img=/home/sys_sgsw/fedora24_test.img
qemu_pidfile=/home/sys_sgsw/qemupid

if [ ! -e "$base_img" ]; then
	echo "Hotplug VM image not found; skipping test"
	exit 0
fi

timing_enter hotplug

timing_enter start_qemu

qemu-img create -b "$base_img" -f qcow2 "$test_img"

qemu-system-x86_64 \
	-daemonize -display none -m 8192 \
	-pidfile "$qemu_pidfile" \
	-hda "$test_img" \
	-net user,hostfwd=tcp::10022-:22 \
	-net nic \
	-cpu host \
	-smp cores=16,sockets=1 \
	--enable-kvm \
	-chardev socket,id=mon0,host=localhost,port=4444,server,nowait \
	-mon chardev=mon0,mode=readline

timing_exit start_qemu

timing_enter wait_for_vm
ssh_vm 'echo ready'
timing_exit wait_for_vm

timing_enter copy_repo
(cd "$rootdir"; tar -cf - .) | (ssh_vm 'tar -xf -')
timing_exit copy_repo

devices_initialization
insert_devices

timing_enter hotplug_test

ssh_vm "examples/nvme/hotplug/hotplug -i 0 -t 25 -n 4 -r 8" &
example_pid=$!

sleep 4
remove_devices
sleep 4
insert_devices
sleep 4
remove_devices
devices_delete

timing_enter wait_for_example
wait $example_pid
timing_exit wait_for_example

trap - SIGINT SIGTERM EXIT

qemupid=`cat "$qemu_pidfile" | awk '{printf $0}'`
kill -9 $qemupid
rm "$qemu_pidfile"
rm "$test_img"

timing_exit hotplug_test

timing_exit hotplug
