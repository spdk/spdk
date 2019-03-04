#!/bin/bash -xe

rootdir=$(readlink -f $(dirname $0))

shopt -s expand_aliases

function on_exit() {
	pkill -9 -F /home/pwodkowx/Private/sandbox/vms/0/qemu.pid
	trap - EXIT
}

trap "on_exit" EXIT ERR

: ${socket=$rootdir/../sandbox/vhost0/vhost.sock}
alias rpc='$rootdir/scripts/rpc.py -s ${socket}'

function start_vm() {
	/home/pwodkowx/Private/sandbox/root/bin/qemu-system-x86_64 \
  	-snapshot -m 512 --enable-kvm -cpu host -smp 4 -vga std -vnc :100 -daemonize \
  	-object memory-backend-file,id=mem,size=512M,mem-path=/dev/hugepages,share=on,prealloc=yes,host-nodes=1,policy=bind \
  	-monitor telnet:127.0.0.1:10002,server,nowait \
  	-numa node,memdev=mem \
  	-pidfile /home/pwodkowx/Private/sandbox/vms/0/qemu.pid \
  	-serial file:/home/pwodkowx/Private/sandbox/vms/0/serial.log \
  	-D /home/pwodkowx/Private/sandbox/vms/0/qemu.log \
  	-drive file=/home/pwodkowx/Private/sandbox/vms/0/os.qcow2,if=none,id=os_disk \
  	-device ide-hd,drive=os_disk,bootindex=0 \
  	-chardev socket,id=vhost0,path=/home/pwodkowx/Private/sandbox/vhost0/vhost.0 \
  	-device vhost-user-scsi-pci,id=scsi0,chardev=vhost0
}

while true; do
	start_vm
	sleep 5
	pkill -9 -F /home/pwodkowx/Private/sandbox/vms/0/qemu.pid
done

trap - EXIT ERR
echo "DONE"
